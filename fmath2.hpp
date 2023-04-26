#pragma once
/**
	@author herumi
	@note modified new BSD license
	http://opensource.org/licenses/BSD-3-Clause
*/
#include <xbyak/xbyak_util.h>
#include <cmath>
#include <vector>
#include <stdexcept>

namespace fmath {

namespace local {

union fi {
	float f;
	uint32_t i;
};

inline float u2f(uint32_t x)
{
	fi fi;
	fi.i = x;
	return fi.f;
}

inline uint32_t f2u(float x)
{
	fi fi;
	fi.f = x;
	return fi.i;
}

#define FMATH_LOG_PRECISE // more exact for |x-1| is small
#define FMATH_LOG_NOT_POSITIVE // return -inf/Nan for x <= 0

struct ConstVar {
	static const size_t expN = 5;
	static const size_t logN = 9;
	float log2; // log(2)
	float log2_e; // log_2(e) = 1 / log2
	float expCoeff[expN]; // near to 1/(i + 1)!
	//
	float logCoeff[logN];
	float logLimit; // 1/32
	float one;
	float c3; // 1/3
	static const size_t L = 4;
	static const size_t LN = 1 << L;
	float logTbl1[LN];
	float logTbl2[LN];
	void init()
	{
		log2 = std::log(2.0f);
		log2_e = 1.0f / log2;
		// maxe=1.938668e-06
		const uint32_t expTbl[expN] = {
			0x3f800000,
			0x3effff12,
			0x3e2aaa56,
			0x3d2b89cc,
			0x3c091331,
		};
		for (size_t i = 0; i < expN; i++) {
			expCoeff[i] = u2f(expTbl[i]);
		}
		const float logTbl[logN] = {
			 1.0, // must be 1
			-0.49999985195974875681242,
			 0.33333220526061677705782,
			-0.25004206220486390058000,
			 0.20010985747510067100077,
			-0.16481566812093889672203,
			 0.13988269735629330763020,
			-0.15049504706005165294002,
			 0.14095711402233803479921,
		};
		for (size_t i = 0; i < logN; i++) {
			logCoeff[i] = logTbl[i];
		}
		logLimit = 1.0 / 16;
		one = 1;
		c3 = 1.0 / 3;
		for (size_t i = 0; i < LN; i++) {
			fi fi;
			fi.i = (127 << 23) | ((i*2+1) << (23 - L - 1));
			logTbl1[i] = 1 / fi.f;
			logTbl2[i] = log(logTbl1[i]);
		}
	}
};

using namespace Xbyak;
using namespace Xbyak::util;

const int freeTbl[] = {
	0, 1, 2, 3, 4, 
#ifndef XBYAK64_WIN
	5, 6,
#endif
};

static const size_t maxFreeN = sizeof(freeTbl)/sizeof(freeTbl[0]);

const int saveTbl[] = {
#ifdef XBYAK64_WIN
	5, 6,
#endif
	7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
};

static const size_t maxSaveN = sizeof(saveTbl)/sizeof(saveTbl[0]);

struct UsedReg {
	size_t pos;
	UsedReg()
		: pos(0)
	{
	}
	int allocRegIdx()
	{
		if (pos < maxFreeN) {
			return freeTbl[pos++];
		}
		if (pos < maxFreeN + maxSaveN) {
			return saveTbl[pos++ - maxFreeN];
		}
		throw std::runtime_error("allocRegIdx");
	}
	int getKeepNum() const
	{
		if (pos < maxFreeN) return 0;
		return pos - maxFreeN;
	}
	size_t getPos() const { return pos; }
};

struct LogParam {
	const Label& constVarL;
	Zmm i127shl23;
	Zmm x7fffff;
	Zmm log2;
	Zmm fNan;
	Zmm fMInf;
	Zmm x7fffffff;
	Zmm one;
	Zmm c2;
	Zmm c3;
	Zmm c4;
	Zmm preciseBoundary;
	Zmm tbl1;
	Zmm tbl2;
	LogParam(const Label& constVarL, UsedReg& usedReg)
		: constVarL(constVarL)
		, i127shl23(usedReg.allocRegIdx())
		, x7fffff(usedReg.allocRegIdx())
		, log2(usedReg.allocRegIdx())
		, fNan(usedReg.allocRegIdx())
		, fMInf(usedReg.allocRegIdx())
		, x7fffffff(usedReg.allocRegIdx())
		, one(usedReg.allocRegIdx())
		, c2(usedReg.allocRegIdx())
		, c3(usedReg.allocRegIdx())
		, c4(usedReg.allocRegIdx())
		, preciseBoundary(usedReg.allocRegIdx())
		, tbl1(usedReg.allocRegIdx())
		, tbl2(usedReg.allocRegIdx())
	{
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
	typedef std::vector<Zmm> Args;
	Xbyak::util::Cpu cpu;
	ConstVar *constVar;
	typedef void (*VecFunc)(float *dst, const float *src, size_t n);
	VecFunc expf_v;
	VecFunc logf_v;
	Code()
		: Xbyak::CodeGenerator(4096 * 2, Xbyak::DontSetProtectRWE)
		, expf_v(0)
		, logf_v(0)
	{
		if (!cpu.has(Xbyak::util::Cpu::tAVX512F)) {
			fprintf(stderr, "AVX-512 is not supported\n");
			return;
		}
		size_t dataSize = sizeof(ConstVar);
		dataSize = (dataSize + 4095) & ~size_t(4095);
		Xbyak::Label constVarL = L();
		constVar = const_cast<ConstVar*>(reinterpret_cast<const ConstVar*>(getCode()));
		constVar->init();
		setSize(dataSize);
		expf_v = getCurr<VecFunc>();
		genExpAVX512(constVarL);
		align(16);
		logf_v = getCurr<VecFunc>();
		genLogAVX512(constVarL);
		setProtectModeRE();
	}
	~Code()
	{
		setProtectModeRW();
	}
	// zm0 = exp(zm0)
	// use zm0, zm1, zm2
	void genExpOneAVX512(const Zmm& log2, const Zmm& log2_e, const Zmm expCoeff[5])
	{
		vmulps(zm0, log2_e);
		vrndscaleps(zm1, zm0, 0); // n = round(x)
		vsubps(zm0, zm1); // a = x - n
		vmulps(zm0, log2); // a *= log2
		vmovaps(zm2, expCoeff[4]);
		vfmadd213ps(zm2, zm0, expCoeff[3]);
		vfmadd213ps(zm2, zm0, expCoeff[2]);
		vfmadd213ps(zm2, zm0, expCoeff[1]);
		vfmadd213ps(zm2, zm0, expCoeff[0]);
		vfmadd213ps(zm2, zm0, expCoeff[0]);
		vscalefps(zm0, zm2, zm1); // zm2 * 2^zm1
	}
	void genExpOneAVX512_2(const Zmm& log2, const Zmm& log2_e, const Zmm expCoeff[5], const Zmm& t0, const Zmm& t1, const Zmm& t2)
	{
		vmulps(zm0, log2_e);
		vmulps(t0, log2_e);
		vrndscaleps(zm1, zm0, 0); // n = round(x)
		vrndscaleps(t1, t0, 0); // n = round(x)
		vsubps(zm0, zm1); // a = x - n
		vsubps(t0, t1); // a = x - n
		vmulps(zm0, log2); // a *= log2
		vmulps(t0, log2); // a *= log2
		vmovaps(zm2, expCoeff[4]);
		vmovaps(t2, expCoeff[4]);
		vfmadd213ps(zm2, zm0, expCoeff[3]);
		vfmadd213ps(t2, t0, expCoeff[3]);
		vfmadd213ps(zm2, zm0, expCoeff[2]);
		vfmadd213ps(t2, t0, expCoeff[2]);
		vfmadd213ps(zm2, zm0, expCoeff[1]);
		vfmadd213ps(t2, t0, expCoeff[1]);
		vfmadd213ps(zm2, zm0, expCoeff[0]);
		vfmadd213ps(t2, t0, expCoeff[0]);
		vfmadd213ps(zm2, zm0, expCoeff[0]);
		vfmadd213ps(t2, t0, expCoeff[0]);
		vscalefps(zm0, zm2, zm1); // zm2 * 2^zm1
		vscalefps(t0, t2, t1); // zm2 * 2^zm1
	}
	// exp_v(float *dst, const float *src, size_t n);
	void genExpAVX512(const Xbyak::Label& constVarL)
	{
#ifdef XBYAK64_WIN
		const int keepRegN = 4 + 3;
#else
		const int keepRegN = 0;
#endif
		using namespace Xbyak;
		util::StackFrame sf(this, 3, util::UseRCX, 64 * keepRegN);
		const Reg64& dst = sf.p[0];
		const Reg64& src = sf.p[1];
		const Reg64& n = sf.p[2];

		// prolog
#ifdef XBYAK64_WIN
		for (int i = 0; i < keepRegN; i++) {
			vmovups(ptr[rsp + 64 * i], Zmm(i + 6));
		}
#endif

		// setup constant
		const Zmm& log2 = zmm3;
		const Zmm& log2_e = zmm4;
		const Zmm expCoeff[] = { zmm5, zmm6, zmm7, zmm8, zmm9 };
		const Zmm& t0 = zm10;
		const Zmm& t1 = zm11;
		const Zmm& t2 = zm12;
		lea(rax, ptr[rip+constVarL]);
		vbroadcastss(log2, ptr[rax + offsetof(ConstVar, log2)]);
		vbroadcastss(log2_e, ptr[rax + offsetof(ConstVar, log2_e)]);
		for (size_t i = 0; i < ConstVar::expN; i++) {
			vbroadcastss(expCoeff[i], ptr[rax + offsetof(ConstVar, expCoeff[0]) + sizeof(float) * i]);
		}

		// main loop
		Label mod32, mod16, exit;
		mov(ecx, n);
		and_(n, ~31u);
		jz(mod32, T_NEAR);
	Label lp = L();
		vmovups(zm0, ptr[src]);
		vmovups(t0, ptr[src + 64]);
		add(src, 128);
		genExpOneAVX512_2(log2, log2_e, expCoeff, t0, t1, t2);
		vmovups(ptr[dst], zm0);
		vmovups(ptr[dst + 64], t0);
		add(dst, 128);
		sub(n, 32);
		jnz(lp);
	L(mod32);
		and_(ecx, 31);
		jz(exit, T_NEAR);
		cmp(ecx, 16);
		jl(mod16);
		vmovups(zm0, ptr[src]);
		add(src, 64);
		genExpOneAVX512(log2, log2_e, expCoeff);
		vmovups(ptr[dst], zm0);
		add(dst, 64);
		sub(ecx, 16);
	L(mod16);
		and_(ecx, 15);
		jz(exit);
		mov(eax, 1);
		shl(eax, cl);
		sub(eax, 1);
		kmovd(k1, eax);
		vmovups(zm0|k1|T_z, ptr[src]);
		genExpOneAVX512(log2, log2_e, expCoeff);
		vmovups(ptr[dst]|k1, zm0|k1);
	L(exit);
		// epilog
#ifdef XBYAK64_WIN
		for (int i = 0; i < keepRegN; i++) {
			vmovups(Zmm(i + 6), ptr[rsp + 64 * i]);
		}
#endif
	}
	/*
		x = 2^n a (1 <= a < 2)
		log x = n * log2 + log a
		L = 4
		d = (f2u(a) & mask(23)) >> (23 - L)
		b = T1[d] = approximate of 1/a
		log b = T2[d]
		c = ab - 1 is near zero
		a = (1 + c) / b
		log a = log(1 + c) - log b
	*/
	// zm0 = log(zm0)
	// use zm0, zm1, zm2
	void genLogOneAVX512(const Args& t, const LogParam& p)
	{
		const Zmm& keepX = t[4];
//int3();
		vmovaps(keepX, t[0]);

		vpsubd(t[1], t[0], p.i127shl23);
		vpsrad(t[1], t[1], 23); // n
		vcvtdq2ps(t[1], t[1]); // int -> float
		vpandd(t[0], t[0], p.x7fffff);
		vpsrad(t[2], t[0], 23 - ConstVar::L); // d
		vpord(t[0], t[0], p.i127shl23); // a
		lea(rax, ptr[rip + p.constVarL]);
		vpermps(t[3], t[2], p.tbl1); // b
		vfmsub213ps(t[0], t[3], p.one); // c = a * b - 1
		vpermps(t[3], t[2], p.tbl2); // log_b
		vfmsub213ps(t[1], p.log2, t[3]); // z = n * log2 - log_b
#ifdef FMATH_LOG_PRECISE // for |x-1| < 1/32
		vsubps(t[2], keepX, p.one); // x-1
		vandps(t[2], t[2], p.x7fffffff); // |x-1|
		vcmpltps(k2, t[2], p.preciseBoundary);
		vsubps(t[0]|k2, keepX, p.one); // c = t[0] = x-1
		vxorps(t[1]|k2, t[1]); // z = 0
#endif

		vmovaps(t[2], t[0]);
		vfmadd213ps(t[2], p.c4, p.c3); // t = c * (-1/4) + (1/3)
		vfmadd213ps(t[2], t[0], p.c2); // t = t * c + (-1/2)
		vfmadd213ps(t[2], t[0], p.one); // t = t * c + 1

		vfmadd213ps(t[0], t[2], t[1]); // c = c * t + z
#ifdef FMATH_LOG_NOT_POSITIVE
		// check x < 0 or x == 0
		const uint8_t neg = 1 << 6;
		const uint8_t zero = (1 << 1) | (1 << 2);
		vfpclassps(k2, keepX, neg);
		vmovaps(t[0]|k2, p.fNan);
		vfpclassps(k2, keepX, zero);
		vmovaps(t[0]|k2, p.fMInf);
#endif
	}
	// use eax
	void setInt(const Zmm& dst, uint32_t x)
	{
		mov(eax, x);
		vpbroadcastd(dst, eax);
	}
	void setFloat(const Zmm& dst, float f)
	{
		setInt(dst, f2u(f));
	}
	// log_v(float *dst, const float *src, size_t n);
	void genLogAVX512(const Xbyak::Label& constVarL)
	{
		UsedReg usedReg;
		int regN = 5;
		Args args;
		for (int i = 0; i < regN; i++) {
			args.push_back(Zmm(usedReg.allocRegIdx()));
		}
		LogParam para(constVarL, usedReg);
		const int keepRegN = usedReg.getKeepNum();
		StackFrame sf(this, 3, UseRCX, 64 * keepRegN);
		const Reg64& dst = sf.p[0];
		const Reg64& src = sf.p[1];
		const Reg64& n = sf.p[2];

		// keep
		for (int i = 0; i < keepRegN; i++) {
			vmovups(ptr[rsp + 64 * i], Zmm(saveTbl[i]));
		}
		// setup constant
		const struct {
			const Zmm& z;
			uint32_t x;
		} intTbl[] = {
			{ para.i127shl23, 127 << 23 },
			{ para.x7fffff, 0x7fffff },
			{ para.fNan, 0x7fc00000 }, // Nan
			{ para.fMInf, 0xff800000 }, // -Inf
			{ para.x7fffffff, 0x7fffffff }, // abs
		};
		for (size_t i = 0; i < sizeof(intTbl)/sizeof(intTbl[0]); i++) {
			setInt(intTbl[i].z, intTbl[i].x);
		}
		const struct {
			const Zmm& z;
			float x;
		} floatTbl[] = {
			{ para.log2, log(2.0f) },
			{ para.one, 1.0f },
			{ para.preciseBoundary, 0.02f },
#if 1
			{ para.c2, -0.49999999f },
			{ para.c3, 0.3333955701f },
			{ para.c4, -0.25008487f },
#else
			{ para.c2, -0.49999909725f },
			{ para.c3, 0.333942362961f },
			{ para.c4, -0.250831127f },
#endif
		};
		for (size_t i = 0; i < sizeof(floatTbl)/sizeof(floatTbl[0]); i++) {
			setFloat(floatTbl[i].z, floatTbl[i].x);
		}
		lea(rax, ptr[rip + para.constVarL]);
		vmovups(para.tbl1, ptr[rax + offsetof(ConstVar, logTbl1)]);
		vmovups(para.tbl2, ptr[rax + offsetof(ConstVar, logTbl2)]);

		// main loop
		Label mod16, exit;
		mov(ecx, n);
		and_(n, ~15u);
		jz(mod16, T_NEAR);
	Label lp = L();
		vmovups(args[0], ptr[src]);
		add(src, 64);
		genLogOneAVX512(args, para);
		vmovups(ptr[dst], args[0]);

		add(dst, 64);
		sub(n, 16);
		jnz(lp);
	L(mod16);
		and_(ecx, 15);
		jz(exit, T_NEAR);
		mov(eax, 1);
		shl(eax, cl);
		sub(eax, 1);
		kmovd(k1, eax);
		vmovups(args[0]|k1|T_z, ptr[src]);
		genLogOneAVX512(args, para);
		vmovups(ptr[dst]|k1, args[0]);
	L(exit);

		// epilog
		for (int i = 0; i < keepRegN; i++) {
			vmovups(Zmm(saveTbl[i]), ptr[rsp + 64 * i]);
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

inline void expf_v(float *dst, const float *src, size_t n)
{
	local::Inst<>::code.expf_v(dst, src, n);
}

inline void logf_v(float *dst, const float *src, size_t n)
{
	local::Inst<>::code.logf_v(dst, src, n);
}

} // fmath
