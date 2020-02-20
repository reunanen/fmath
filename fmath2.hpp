#pragma once
/**
	@author herumi
	@note modified new BSD license
	http://opensource.org/licenses/BSD-3-Clause
*/
#include <xbyak/xbyak_util.h>
#include <cmath>

namespace fmath2 {

namespace local {

union fi {
	float f;
	uint32_t i;
};

inline float cvt(uint32_t x)
{
	fi fi;
	fi.i = x;
	return fi.f;
}

struct ConstVar {
	static const size_t TaylerN = 5;
	float expMin; // exp(expMin) = 0
	float expMax; // exp(expMax) = inf
	float log2; // log(2)
	float log2_e; // log_2(e) = 1 / log2
	float expCoeff[TaylerN]; // near to 1/(i + 1)!
	void init()
	{
		expMin = cvt(0xc2aeac50);
		expMax = cvt(0x42b17218);
		log2 = std::log(2.0f);
		log2_e = 1.0f / log2;
#if 0
		// maxe=4.888831e-06
		float z = 1;
		for (size_t i = 0; i < TaylerN; i++) {
			expCoeff[i] = z;
			z /= (i + 2);
		}
#else
		// maxe=1.938668e-06
		const uint32_t tbl[TaylerN] = {
			0x3f800000,
			0x3effff12,
			0x3e2aaa56,
			0x3d2b89cc,
			0x3c091331,
		};
		for (size_t i = 0; i < TaylerN; i++) {
			expCoeff[i] = cvt(tbl[i]);
		}
#endif
	}
};

/*
The constans expCoeff are generated by Maple.
f := x->A+B*x+C*x^2+D*x^3+E*x^4+F*x^5;
g:=int((f(x)-exp(x))^2,x=-L..L);
sols:=solve({diff(g,A)=0,diff(g,B)=0,diff(g,C)=0,diff(g,D)=0,diff(g,E)=0,diff(g,F)=0},{A,B,C,D,E,F});
Digits:=1000;
s:=eval(sols,L=log(2)/2);
evalf(s,20);
*/
struct Code : public Xbyak::CodeGenerator {
	typedef Xbyak::Zmm Zmm;
	Xbyak::util::Cpu cpu;
	ConstVar *constVar;
	typedef void (*VecFunc)(float *dst, const float *src, size_t n);
	VecFunc expf_v;
	Code()
		: Xbyak::CodeGenerator(4096 * 2, Xbyak::DontSetProtectRWE)
	{
		if (!cpu.has(Xbyak::util::Cpu::tAVX512F)) {
			fprintf(stderr, "AVX-512 is not supported\n");
			exit(1);
		}
		size_t dataSize = sizeof(ConstVar);
		dataSize = (dataSize + 4095) & ~size_t(4095);
		Xbyak::Label expDataL = L();
		constVar = (ConstVar*)getCode();
		constVar->init();
		setSize(dataSize);
		expf_v = getCurr<VecFunc>();
		genExp(expDataL);
		setProtectModeRE();
	}
	~Code()
	{
		setProtectModeRW();
	}
	// zm0 = exp(zm0)
	// use zm0, zm1, zm2
	void genOneExp(const Zmm& i127, const Zmm& expMin, const Zmm& expMax, const Zmm& log2, const Zmm& log2_e, const Zmm expCoeff[5])
	{
		vminps(zm0, expMax);
		vmaxps(zm0, expMin);
		vmulps(zm0, log2_e);
		vrndscaleps(zm1, zm0, 0); // n = round(x)
		vsubps(zm0, zm1); // a
		vcvtps2dq(zm1, zm1);
		vmulps(zm0, log2);
		vpaddd(zm1, zm1, i127);
		vpslld(zm1, zm1, 23); // fi.f
		vmovaps(zm2, expCoeff[4]);
		vfmadd213ps(zm2, zm0, expCoeff[3]);
		vfmadd213ps(zm2, zm0, expCoeff[2]);
		vfmadd213ps(zm2, zm0, expCoeff[1]);
		vfmadd213ps(zm2, zm0, expCoeff[0]);
		vfmadd213ps(zm2, zm0, expCoeff[0]);
		vmulps(zm0, zm2, zm1);
	}
	// exp_v(float *dst, const float *src, size_t n);
	void genExp(const Xbyak::Label& expDataL)
	{
		const int keepRegN = 7;
		using namespace Xbyak;
		util::StackFrame sf(this, 3, util::UseRCX, 64 * keepRegN);
		const Reg64& dst = sf.p[0];
		const Reg64& src = sf.p[1];
		const Reg64& n = sf.p[2];

		// prolog
#ifdef XBYAK64_WIN
		vmovups(ptr[rsp + 64 * 0], zm6);
		vmovups(ptr[rsp + 64 * 1], zm7);
#endif
		for (int i = 2; i < keepRegN; i++) {
			vmovups(ptr[rsp + 64 * i], Zmm(i + 6));
		}

		// setup constant
		const Zmm& i127 = zmm3;
		const Zmm& expMin = zmm4;
		const Zmm& expMax = zmm5;
		const Zmm& log2 = zmm6;
		const Zmm& log2_e = zmm7;
		const Zmm expCoeff[] = { zmm8, zmm9, zmm10, zmm11, zmm12 };
		mov(eax, 127);
		vpbroadcastd(i127, eax);
		vpbroadcastd(expMin, ptr[rip + expDataL + (int)offsetof(ConstVar, expMin)]);
		vpbroadcastd(expMax, ptr[rip + expDataL + (int)offsetof(ConstVar, expMax)]);
		vpbroadcastd(log2, ptr[rip + expDataL + (int)offsetof(ConstVar, log2)]);
		vpbroadcastd(log2_e, ptr[rip + expDataL + (int)offsetof(ConstVar, log2_e)]);
		for (size_t i = 0; i < ConstVar::TaylerN; i++) {
			vpbroadcastd(expCoeff[i], ptr[rip + expDataL + (int)(offsetof(ConstVar, expCoeff[0]) + sizeof(float) * i)]);
		}

		// main loop
		Label mod16, exit;
		mov(ecx, n);
		and_(n, ~15);
		jz(mod16);
		align(16);
	Label lp = L();
		vmovups(zm0, ptr[src]);
		add(src, 64);
		genOneExp(i127, expMin, expMax, log2, log2_e, expCoeff);
		vmovups(ptr[dst], zm0);
		add(dst, 64);
		sub(n, 16);
		jnz(lp);
	L(mod16);
		and_(ecx, 15);
		jz(exit);
		mov(eax, 1);
		shl(eax, cl);
		sub(eax, 1);
		kmovd(k1, eax);
		vmovups(zm0|k1|T_z, ptr[src]);
		genOneExp(i127, expMin, expMax, log2, log2_e, expCoeff);
		vmovups(ptr[dst]|k1, zm0|k1);
	L(exit);
		// epilog
#ifdef XBYAK64_WIN
		vmovups(zm6, ptr[rsp + 64 * 0]);
		vmovups(zm7, ptr[rsp + 64 * 1]);
#endif
		for (int i = 2; i < keepRegN; i++) {
			vmovups(Zmm(i + 6), ptr[rsp + 64 * i]);
		}
	}
};

template<size_t dummy = 0>
struct Inst {
	static const Code code;
};

template<size_t dummy>
MIE_ALIGN(32) const Code Inst<dummy>::code;

} // fmath::local

inline float split(int *pn, float x)
{
	int n;
	if (x >= 0) {
		n = int(x + 0.5f);
	} else {
		n = int(x - 0.5f);
	}
	*pn = n;
	return x - n;
}

inline float expfC(float x)
{
	const local::ConstVar& C = *local::Inst<>::code.constVar;
	x = (std::min)(x, C.expMax);
	x = (std::max)(x, C.expMin);
	x *= C.log2_e;
	int n;
	float a = split(&n, x);
	/* |a| <= 0.5 */
	a *= C.log2;
	/* |a| <= 0.3466 */
	local::fi fi;
	fi.i = (n + 127) << 23; // 2^n
	/*
		e^a = 1 + a + a^2/2! + a^3/3! + a^4/4! + a^5/5!
		= 1 + a(1 + a(1/2! + a(1/3! + a(1/4! + a/5!))))
	*/
	x = C.expCoeff[4];
	x = a * x + C.expCoeff[3];
	x = a * x + C.expCoeff[2];
	x = a * x + C.expCoeff[1];
	x = a * x + C.expCoeff[0];
	x = a * x + C.expCoeff[0];
	return x * fi.f;
}

inline void expf_vC(float *dst, const float *src, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		dst[i] = expfC(src[i]);
	}
}

inline void expf_v(float *dst, const float *src, size_t n)
{
	local::Inst<>::code.expf_v(dst, src, n);
}

} // fmath2
