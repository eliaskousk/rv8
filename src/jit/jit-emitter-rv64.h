//
//  jit-emitter-rv64.h
//

#ifndef rv_jit_emitter_rv64_h
#define rv_jit_emitter_rv64_h

#include <bitset>

namespace riscv {

	using namespace asmjit;

	struct jit_isa_rv64
	{
		std::bitset<2048> supported_ops;

		jit_isa_rv64()
		{
			static const int ops[] = {
				rv_op_auipc,
				rv_op_add,
				rv_op_sub,
				rv_op_mul,
				rv_op_mulh,
				rv_op_mulhu,
				rv_op_mulhsu,
				rv_op_div,
				rv_op_rem,
				rv_op_divu,
				rv_op_remu,
				rv_op_slt,
				rv_op_sltu,
				rv_op_and,
				rv_op_or,
				rv_op_xor,
				rv_op_sll,
				rv_op_srl,
				rv_op_sra,
				rv_op_addw,
				rv_op_subw,
				rv_op_mulw,
				rv_op_divw,
				rv_op_remw,
				rv_op_divuw,
				rv_op_remuw,
				rv_op_sllw,
				rv_op_srlw,
				rv_op_sraw,
				rv_op_addi,
				rv_op_slti,
				rv_op_sltiu,
				rv_op_andi,
				rv_op_ori,
				rv_op_xori,
				rv_op_slli,
				rv_op_srli,
				rv_op_srai,
				rv_op_addiw,
				rv_op_slliw,
				rv_op_srliw,
				rv_op_sraiw,
				rv_op_bne,
				rv_op_beq,
				rv_op_blt,
				rv_op_bge,
				rv_op_bltu,
				rv_op_bgeu,
				rv_op_ld,
				rv_op_lw,
				rv_op_lwu,
				rv_op_lh,
				rv_op_lhu,
				rv_op_lb,
				rv_op_lbu,
				rv_op_sd,
				rv_op_sw,
				rv_op_sh,
				rv_op_sb,
				rv_op_lui,
				rv_op_jal,
				rv_op_jalr,
				jit_op_la,
				jit_op_call,
				jit_op_zextw,
				jit_op_addiwz,
				jit_op_rorwi_rr,
				jit_op_rorwi_lr,
				jit_op_rordi_rr,
				jit_op_rordi_lr,
				jit_op_auipc_lw,
				jit_op_auipc_ld,
				rv_op_illegal
			};
			const int *op = ops;
			while (*op != rv_op_illegal) supported_ops.set(*op++);
		}
	};

	template <typename P, bool MEMREG = false>
	struct jit_emitter_rv64
	{
		typedef P processor_type;
		typedef typename P::decode_type decode_type;

		#define proc_offset(member) offsetof(typename P::processor_type, member)

		P &proc;
		X86Assembler as;
		CodeHolder &code;
		mmu_ops ops, ops_wrap;
		TraceLookup lookup_trace_slow;
		TraceLookup lookup_trace_fast;
		std::map<addr_t,Label> labels;
		std::map<addr_t,Label> jmp_tramp_labels;
		std::map<addr_t,Label> exit_tramp_labels;
		std::map<addr_t,std::vector<Label>> jmp_fixup_labels;
		std::vector<addr_t> callstack;
		u64 term_pc;
		int instret;
		bool use_mmu;
		Label start, term;

		jit_emitter_rv64(P &proc, CodeHolder &code, mmu_ops &ops, TraceLookup lookup_trace_slow, TraceLookup lookup_trace_fast)
			: proc(proc), as(&code), code(code), ops(ops),
			  lookup_trace_slow(lookup_trace_slow),
			  lookup_trace_fast(lookup_trace_fast),
			  term_pc(0), instret(0), use_mmu(false)
		{}

		void log_trace(const char* fmt, ...)
		{
			if (proc.log & proc_log_jit_trace) {
				char buf[128];
				va_list arg;
				va_start(arg, fmt);
				vsnprintf(buf, sizeof(buf), fmt, arg);
				va_end(arg);
				printf("%s\n", buf);
			}
		}

		virtual bool handleError(Error err, const char* message, CodeEmitter* origin) {
			log_trace("%s", message);
			return false;
		}

		static int x86_reg(int rd)
		{
			if (MEMREG) {
				return -1; /* all registers are memory backed */
			}
			switch (rd) {
				case rv_ireg_zero: return 0;
				case rv_ireg_ra: return 2;  /* rdx */
				case rv_ireg_sp: return 3;  /* rbx */
				case rv_ireg_t0: return 6;  /* rsi */
				case rv_ireg_t1: return 7;  /* rdi */
				case rv_ireg_a0: return 8;  /* r8  */
				case rv_ireg_a1: return 9;  /* r9  */
				case rv_ireg_a2: return 10; /* r10 */
				case rv_ireg_a3: return 11; /* r11 */
				case rv_ireg_a4: return 12; /* r12 */
				case rv_ireg_a5: return 13; /* r13 */
				case rv_ireg_a6: return 14; /* r14 */
				case rv_ireg_a7: return 15; /* r15 */
			}
			return -1;
		}

		const char* rbp_reg_str_d(int reg)
		{
			static char buf[32];
			snprintf(buf, sizeof(buf), "dword ptr [rbp + %lu]", proc_offset(ireg) + reg * (P::xlen >> 3));
			return buf;
		}

		const X86Mem rbp_reg_d(int reg)
		{
			return x86::dword_ptr(x86::rbp, proc_offset(ireg) + reg * (P::xlen >> 3));
		}

		const char* rbp_reg_str_q(int reg)
		{
			static char buf[32];
			snprintf(buf, sizeof(buf), "qword ptr [rbp + %lu]", proc_offset(ireg) + reg * (P::xlen >> 3));
			return buf;
		}

		const X86Mem rbp_reg_q(int reg)
		{
			return x86::qword_ptr(x86::rbp, proc_offset(ireg) + reg * (P::xlen >> 3));
		}

		void commit_instret()
		{
			if (proc.update_instret && instret > 0) {
				as.add(x86::qword_ptr(x86::rbp, proc_offset(instret)), Imm(instret));
				instret = 0;
			}
		}

		void emit_prolog()
		{
			if (!MEMREG) {
				as.push(x86::r12);
				as.push(x86::r13);
				as.push(x86::r14);
				as.push(x86::r15);
				as.push(x86::rbx);
			}
			as.push(x86::rbp);
			as.mov(x86::rbp, x86::rdi);
			if (!MEMREG) {
				as.mov(x86::rdx, rbp_reg_q(rv_ireg_ra));
				as.mov(x86::rbx, rbp_reg_q(rv_ireg_sp));
				as.mov(x86::rsi, rbp_reg_q(rv_ireg_t0));
				as.mov(x86::rdi, rbp_reg_q(rv_ireg_t1));
				as.mov(x86::r8,  rbp_reg_q(rv_ireg_a0));
				as.mov(x86::r9,  rbp_reg_q(rv_ireg_a1));
				as.mov(x86::r10, rbp_reg_q(rv_ireg_a2));
				as.mov(x86::r11, rbp_reg_q(rv_ireg_a3));
				as.mov(x86::r12, rbp_reg_q(rv_ireg_a4));
				as.mov(x86::r13, rbp_reg_q(rv_ireg_a5));
				as.mov(x86::r14, rbp_reg_q(rv_ireg_a6));
				as.mov(x86::r15, rbp_reg_q(rv_ireg_a7));
			}

			instret = 0;
		}

		void emit_epilog()
		{
			commit_instret();

			if (!MEMREG) {
				as.mov(rbp_reg_q(rv_ireg_ra), x86::rdx);
				as.mov(rbp_reg_q(rv_ireg_sp), x86::rbx);
				as.mov(rbp_reg_q(rv_ireg_t0), x86::rsi);
				as.mov(rbp_reg_q(rv_ireg_t1), x86::rdi);
				as.mov(rbp_reg_q(rv_ireg_a0), x86::r8);
				as.mov(rbp_reg_q(rv_ireg_a1), x86::r9);
				as.mov(rbp_reg_q(rv_ireg_a2), x86::r10);
				as.mov(rbp_reg_q(rv_ireg_a3), x86::r11);
				as.mov(rbp_reg_q(rv_ireg_a4), x86::r12);
				as.mov(rbp_reg_q(rv_ireg_a5), x86::r13);
				as.mov(rbp_reg_q(rv_ireg_a6), x86::r14);
				as.mov(rbp_reg_q(rv_ireg_a7), x86::r15);
			}
			as.pop(x86::rbp);
			if (!MEMREG) {
				as.pop(x86::rbx);
				as.pop(x86::r15);
				as.pop(x86::r14);
				as.pop(x86::r13);
				as.pop(x86::r12);
			}
			as.ret();

			for (auto &jtl : jmp_tramp_labels) {
				as.bind(jtl.second);
				emit_pc(jtl.first);
				as.jmp(Imm(func_address(lookup_trace_fast)));
			}

			for (auto &jtl : exit_tramp_labels) {
				as.bind(jtl.second);
				emit_pc(jtl.first);
				as.jmp(term);
			}
		}

		TraceLookup create_trace_lookup(JitRuntime &rt)
		{
			auto lookup_slow = as.newLabel();
			auto lookup_fail = as.newLabel();

			u32 mask = (P::trace_l1_size - 1) << 1;

			/* fast path lookup cache pc -> trace fn */
			as.mov(x86::rcx, x86::qword_ptr(x86::rbp, proc_offset(pc)));
			as.mov(x86::rax, x86::rcx);
			as.and_(x86::rcx, Imm(mask));
			as.cmp(x86::rax, x86::qword_ptr(x86::rbp, x86::rcx, 4, proc_offset(trace_pc)));
			as.jne(lookup_slow);
			as.jmp(x86::qword_ptr(x86::rbp, x86::rcx, 4, proc_offset(trace_fn)));

			/* slow path lookup cache pc -> trace fn */
			as.bind(lookup_slow);
			if (!MEMREG) {
				as.mov(rbp_reg_q(rv_ireg_ra), x86::rdx);
				as.mov(rbp_reg_q(rv_ireg_sp), x86::rbx);
				as.mov(rbp_reg_q(rv_ireg_t0), x86::rsi);
				as.mov(rbp_reg_q(rv_ireg_t1), x86::rdi);
				as.mov(rbp_reg_q(rv_ireg_a0), x86::r8);
				as.mov(rbp_reg_q(rv_ireg_a1), x86::r9);
				as.mov(rbp_reg_q(rv_ireg_a2), x86::r10);
				as.mov(rbp_reg_q(rv_ireg_a3), x86::r11);
			}
			as.mov(x86::rdi, x86::rax);
			as.call(Imm(func_address(lookup_trace_slow)));
			as.test(x86::rax, x86::rax);
			as.jz(lookup_fail);
			as.mov(x86::rcx, x86::qword_ptr(x86::rbp, proc_offset(pc)));
			as.mov(x86::rdx, x86::rcx);
			as.and_(x86::rcx, Imm(mask));
			as.mov(x86::qword_ptr(x86::rbp, x86::rcx, 4, proc_offset(trace_fn)), x86::rax);
			as.mov(x86::qword_ptr(x86::rbp, x86::rcx, 4, proc_offset(trace_pc)), x86::rdx);
			if (!MEMREG) {
				as.mov(x86::rdx, rbp_reg_q(rv_ireg_ra));
				as.mov(x86::rbx, rbp_reg_q(rv_ireg_sp));
				as.mov(x86::rsi, rbp_reg_q(rv_ireg_t0));
				as.mov(x86::rdi, rbp_reg_q(rv_ireg_t1));
				as.mov(x86::r8,  rbp_reg_q(rv_ireg_a0));
				as.mov(x86::r9,  rbp_reg_q(rv_ireg_a1));
				as.mov(x86::r10, rbp_reg_q(rv_ireg_a2));
				as.mov(x86::r11, rbp_reg_q(rv_ireg_a3));
			}
			as.jmp(x86::rax);

			/* fail path, return to emulator */
			as.bind(lookup_fail);
			if (!MEMREG) {
				as.mov(rbp_reg_q(rv_ireg_a4), x86::r12);
				as.mov(rbp_reg_q(rv_ireg_a5), x86::r13);
				as.mov(rbp_reg_q(rv_ireg_a6), x86::r14);
				as.mov(rbp_reg_q(rv_ireg_a7), x86::r15);
			}
			as.pop(x86::rbp);
			if (!MEMREG) {
				as.pop(x86::rbx);
				as.pop(x86::r15);
				as.pop(x86::r14);
				as.pop(x86::r13);
				as.pop(x86::r12);
			}
			as.ret();

			Error err = rt.add(&lookup_trace_fast, &code);
			if (err) panic("failed to create trace lookup function");
			return lookup_trace_fast;
		}

		void save_volatile()
		{
			if (MEMREG) return;
			as.mov(rbp_reg_q(rv_ireg_ra), x86::rdx);
			as.mov(rbp_reg_q(rv_ireg_t0), x86::rsi);
			as.mov(rbp_reg_q(rv_ireg_t1), x86::rdi);
			as.mov(rbp_reg_q(rv_ireg_a0), x86::r8);
			as.mov(rbp_reg_q(rv_ireg_a1), x86::r9);
			as.mov(rbp_reg_q(rv_ireg_a2), x86::r10);
			as.mov(rbp_reg_q(rv_ireg_a3), x86::r11);
		}

		void restore_volatile()
		{
			if (MEMREG) return;
			as.mov(x86::rdx, rbp_reg_q(rv_ireg_ra));
			as.mov(x86::rsi, rbp_reg_q(rv_ireg_t0));
			as.mov(x86::rdi, rbp_reg_q(rv_ireg_t1));
			as.mov(x86::r8, rbp_reg_q(rv_ireg_a0));
			as.mov(x86::r9, rbp_reg_q(rv_ireg_a1));
			as.mov(x86::r10, rbp_reg_q(rv_ireg_a2));
			as.mov(x86::r11, rbp_reg_q(rv_ireg_a3));
		}

		mmu_ops create_load_store(JitRuntime &rt)
		{
			Label lb = as.newLabel();
			as.align(kAlignCode, 16);
			as.bind(lb);
			save_volatile();
			as.mov(x86::rdi, x86::rax);
			as.call(Imm(func_address(ops.lb)));
			restore_volatile();
			as.ret();

			Label lh = as.newLabel();
			as.align(kAlignCode, 16);
			as.bind(lh);
			save_volatile();
			as.mov(x86::rdi, x86::rax);
			as.call(Imm(func_address(ops.lh)));
			restore_volatile();
			as.ret();

			Label lw = as.newLabel();
			as.align(kAlignCode, 16);
			as.bind(lw);
			save_volatile();
			as.mov(x86::rdi, x86::rax);
			as.call(Imm(func_address(ops.lw)));
			restore_volatile();
			as.ret();

			Label ld = as.newLabel();
			as.align(kAlignCode, 16);
			as.bind(ld);
			save_volatile();
			as.mov(x86::rdi, x86::rax);
			as.call(Imm(func_address(ops.ld)));
			restore_volatile();
			as.ret();

			Label sb = as.newLabel();
			as.align(kAlignCode, 16);
			as.bind(sb);
			save_volatile();
			as.mov(x86::rdi, x86::rax);
			as.mov(x86::rsi, x86::rcx);
			as.call(Imm(func_address(ops.sb)));
			restore_volatile();
			as.ret();

			Label sh = as.newLabel();
			as.align(kAlignCode, 16);
			as.bind(sh);
			save_volatile();
			as.mov(x86::rdi, x86::rax);
			as.mov(x86::rsi, x86::rcx);
			as.call(Imm(func_address(ops.sh)));
			restore_volatile();
			as.ret();

			Label sw = as.newLabel();
			as.align(kAlignCode, 16);
			as.bind(sw);
			save_volatile();
			as.mov(x86::rdi, x86::rax);
			as.mov(x86::rsi, x86::rcx);
			as.call(Imm(func_address(ops.sw)));
			restore_volatile();
			as.ret();

			Label sd = as.newLabel();
			as.align(kAlignCode, 16);
			as.bind(sd);
			save_volatile();
			as.mov(x86::rdi, x86::rax);
			as.mov(x86::rsi, x86::rcx);
			as.call(Imm(func_address(ops.sd)));
			restore_volatile();
			as.ret();

			TraceFunc fn;
			Error err = rt.add(&fn, &code);
			if (err) panic("failed to load store functions");

			mmu_ops ops = {
				.lb = func_address_offset<lb_fn>(fn, code.getLabelOffset(lb)),
				.lh = func_address_offset<lh_fn>(fn, code.getLabelOffset(lh)),
				.lw = func_address_offset<lw_fn>(fn, code.getLabelOffset(lw)),
				.ld = func_address_offset<ld_fn>(fn, code.getLabelOffset(ld)),
				.sb = func_address_offset<sb_fn>(fn, code.getLabelOffset(sb)),
				.sh = func_address_offset<sh_fn>(fn, code.getLabelOffset(sh)),
				.sw = func_address_offset<sw_fn>(fn, code.getLabelOffset(sw)),
				.sd = func_address_offset<sd_fn>(fn, code.getLabelOffset(sd))
			};

			return ops;
		}

		void begin()
		{
			term = as.newLabel();
			start = as.newLabel();
			as.bind(start);
		}

		void end()
		{
			if (term_pc) {
				emit_pc(term_pc);
				log_trace("\t# 0x%016llx", term_pc);
			}
			as.bind(term);
		}

		void emit_pc(uintptr_t new_pc)
		{
			if (new_pc < std::numeric_limits<u32>::max()) {
				as.mov(x86::qword_ptr(x86::rbp, proc_offset(pc)), Imm(new_pc));
			} else {
				as.mov(x86::rax, Imm(new_pc));
				as.mov(x86::qword_ptr(x86::rbp, proc_offset(pc)), x86::rax);
			}
		}

		void emit_sx_32_rd(decode_type &dec)
		{
			int rdx = x86_reg(dec.rd);
			if (rdx > 0) {
				as.movsxd(x86::gpq(rdx), x86::gpd(rdx));
			} else {
				as.movsxd(x86::rcx, rbp_reg_d(dec.rd));
				as.mov(rbp_reg_q(dec.rd), x86::rcx);
			}
		}

		void emit_zero_rd(decode_type &dec)
		{
			int rdx = x86_reg(dec.rd);
			if (rdx > 0) {
				as.xor_(x86::gpd(rdx), x86::gpd(rdx));
			} else {
				as.mov(rbp_reg_q(dec.rd), Imm(0));
			}
		}

		void emit_mv_rax_rs1(decode_type &dec)
		{
			int rs1x = x86_reg(dec.rs1);
			if (rs1x > 0) {
				as.mov(x86::rax, x86::gpq(rs1x));
			} else {
				as.mov(x86::rax, rbp_reg_q(dec.rs1));
			}
		}

		void emit_mv_rd_rs1(decode_type &dec)
		{
			if (dec.rd == dec.rs1) return;
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1);
			if (rdx > 0) {
				if (rs1x > 0) {
					as.mov(x86::gpq(rdx), x86::gpq(rs1x));
				} else {
					as.mov(x86::gpq(rdx), rbp_reg_q(dec.rs1));
				}
			} else {
				if (rs1x > 0) {
					as.mov(rbp_reg_q(dec.rd), x86::gpq(rs1x));
				} else {
					as.mov(x86::rax, rbp_reg_q(dec.rs1));
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}
		}

		void emit_mv_rd_rs1_32(decode_type &dec)
		{
			if (dec.rd == dec.rs1) return;
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1);
			if (rdx > 0) {
				if (rs1x > 0) {
					as.mov(x86::gpd(rdx), x86::gpd(rs1x));
				} else {
					as.mov(x86::gpd(rdx), rbp_reg_d(dec.rs1));
				}
			} else {
				if (rs1x > 0) {
					as.mov(rbp_reg_d(dec.rd), x86::gpd(rs1x));
				} else {
					as.mov(x86::eax, rbp_reg_d(dec.rs1));
					as.mov(rbp_reg_d(dec.rd), x86::eax);
				}
			}
		}

		void emit_mv_rd_rs2(decode_type &dec)
		{
			if (dec.rd == dec.rs2) return;
			int rdx = x86_reg(dec.rd), rs2x = x86_reg(dec.rs2);
			if (rdx > 0) {
				if (rs2x > 0) {
					as.mov(x86::gpq(rdx), x86::gpq(rs2x));
				} else {
					as.mov(x86::gpq(rdx), rbp_reg_q(dec.rs2));
				}
			} else {
				if (rs2x > 0) {
					as.mov(rbp_reg_q(dec.rd), x86::gpq(rs2x));
				} else {
					as.mov(x86::rax, rbp_reg_q(dec.rs2));
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}
		}

		void emit_mv_rd_rs1_sx_32(decode_type &dec)
		{
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1);
			if (rdx > 0) {
				if (rs1x > 0) {
					as.movsxd(x86::gpq(rdx), x86::gpd(rs1x));
				} else {
					as.movsxd(x86::gpq(rdx), rbp_reg_d(dec.rs1));
				}
			} else {
				if (rs1x > 0) {
					as.movsxd(x86::rax, x86::gpd(rs1x));
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				} else {
					as.movsxd(x86::rax, rbp_reg_d(dec.rs1));
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}
		}

		void emit_mv_rd_rs2_sx_32(decode_type &dec)
		{
			int rdx = x86_reg(dec.rd), rs2x = x86_reg(dec.rs2);
			if (rdx > 0) {
				if (rs2x > 0) {
					as.movsxd(x86::gpq(rdx), x86::gpd(rs2x));
				} else {
					as.mov(x86::gpq(rdx), rbp_reg_d(dec.rs2));
				}
			} else {
				if (rs2x > 0) {
					as.movsxd(x86::rax, x86::gpd(rs2x));
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				} else {
					as.movsxd(x86::rax, rbp_reg_d(dec.rs2));
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}
		}

		void emit_mv_cl_rs2(decode_type &dec)
		{
			int rs2x = x86_reg(dec.rs2);
			if (rs2x > 0) {
				as.mov(x86::ecx, x86::gpd(rs2x));
			} else {
				as.mov(x86::ecx, rbp_reg_d(dec.rs2));
			}
		}

		bool emit_auipc(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd);
			if (dec.rd == rv_ireg_zero) {
				// nop
			} else {
				if (rdx > 0) {
					as.mov(x86::gpq(rdx), Imm(dec.pc + dec.imm));
				} else {
					as.mov(rbp_reg_q(dec.rd), Imm(dec.pc + dec.imm));
				}
			}
			return true;
		}

		bool emit_add(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_mv_rd_rs2(dec);
			}
			else if (dec.rs2 == rv_ireg_zero) {
				emit_mv_rd_rs1(dec);
			}
			else if (dec.rd == dec.rs1) {
				if (rdx > 0) {
					if (rs2x > 0) {
						as.add(x86::gpq(rdx), x86::gpq(rs2x));
					} else {
						as.add(x86::gpq(rdx), rbp_reg_q(dec.rs2));
					}
				} else {
					if (rs2x > 0) {
						as.add(rbp_reg_q(dec.rd), x86::gpq(rs2x));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs2));
						as.add(rbp_reg_q(dec.rd), x86::rax);
					}
				}
			}
			else if (dec.rd == dec.rs2) {
				if (rdx > 0) {
					if (rs1x > 0) {
						as.add(x86::gpq(rdx), x86::gpq(rs1x));
					} else {
						as.add(x86::gpq(rdx), rbp_reg_q(dec.rs1));
					}
				} else {
					if (rs1x > 0) {
						as.add(rbp_reg_q(dec.rd), x86::gpq(rs1x));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.add(rbp_reg_q(dec.rd), x86::rax);
					}
				}
			}
			else if (rdx > 0 && rs1x > 0 && rs2x > 0) {
				as.lea(x86::gpq(rdx), x86::qword_ptr(x86::gpq(rs1x), x86::gpq(rs2x)));
			}
			else if (rdx < 0 && (rs1x < 0 || rs2x < 0)) {
				emit_mv_rax_rs1(dec);
				if (rs2x > 0) {
					as.add(x86::rax, x86::gpq(rs2x));
				} else {
					as.add(x86::rax, rbp_reg_q(dec.rs2));
				}
				as.mov(rbp_reg_q(dec.rd), x86::rax);
			}
			else {
				emit_mv_rd_rs1(dec);
				if (rdx > 0) {
					if (rs2x > 0) {
						as.add(x86::gpq(rdx), x86::gpq(rs2x));
					} else {
						as.add(x86::gpq(rdx), rbp_reg_q(dec.rs2));
					}
				} else {
					as.add(rbp_reg_q(dec.rd), x86::gpq(rs2x));
				}
			}
			return true;
		}

		bool emit_sub(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_mv_rd_rs2(dec);
				if (rdx > 0) {
					as.neg(x86::gpq(rdx));
				} else {
					as.neg(rbp_reg_q(dec.rd));
				}
			}
			else if (dec.rs2 == rv_ireg_zero) {
				emit_mv_rd_rs1(dec);
			}
			else if (dec.rd == dec.rs1) {
				if (rdx > 0) {
					if (rs2x > 0) {
						as.sub(x86::gpq(rdx), x86::gpq(rs2x));
					} else {
						as.sub(x86::gpq(rdx), rbp_reg_q(dec.rs2));
					}
				} else {
					if (rs2x > 0) {
						as.sub(rbp_reg_q(dec.rd), x86::gpq(rs2x));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs2));
						as.sub(rbp_reg_q(dec.rd), x86::rax);
					}
				}
			}
			else if (dec.rd == dec.rs2) {
				if (rdx > 0) {
					if (rs1x > 0) {
						as.sub(x86::gpq(rdx), x86::gpq(rs1x));
					} else {
						as.sub(x86::gpq(rdx), rbp_reg_q(dec.rs1));
					}
				} else {
					if (rs1x > 0) {
						as.sub(rbp_reg_q(dec.rd), x86::gpq(rs1x));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.sub(rbp_reg_q(dec.rd), x86::rax);
					}
				}
				if (rdx > 0) {
					as.neg(x86::gpq(rdx));
				} else {
					as.neg(rbp_reg_q(dec.rd));
				}
			}
			else if (rdx < 0 && (rs1x < 0 || rs2x < 0)) {
				emit_mv_rax_rs1(dec);
				if (rs2x > 0) {
					as.sub(x86::rax, x86::gpq(rs2x));
				} else {
					as.sub(x86::rax, rbp_reg_q(dec.rs2));
				}
				as.mov(rbp_reg_q(dec.rd), x86::rax);
			}
			else {
				emit_mv_rd_rs1(dec);
				if (rdx > 0) {
					if (rs2x > 0) {
						as.sub(x86::gpq(rdx), x86::gpq(rs2x));
					} else {
						as.sub(x86::gpq(rdx), rbp_reg_q(dec.rs2));
					}
				} else {
					as.sub(rbp_reg_q(dec.rd), x86::gpq(rs2x));
				}
			}
			return true;
		}

		bool emit_mul(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero || dec.rs2 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else if (dec.rd == dec.rs1 && rdx > 0) {
				if (rs2x > 0) {
					as.imul(x86::gpq(rdx), x86::gpq(rs2x));
				} else {
					as.imul(x86::gpq(rdx), rbp_reg_q(dec.rs2));
				}
			}
			else if (dec.rd == dec.rs2 && rdx > 0) {
				if (rs1x > 0) {
					as.imul(x86::gpq(rdx), x86::gpq(rs1x));
				} else {
					as.imul(x86::gpq(rdx), rbp_reg_q(dec.rs1));
				}
			}
			else if (rdx < 0) {
				emit_mv_rax_rs1(dec);
				if (rs2x > 0) {
					as.imul(x86::rax, x86::gpq(rs2x));
				} else {
					as.imul(x86::rax, rbp_reg_q(dec.rs2));
				}
				as.mov(rbp_reg_q(dec.rd), x86::rax);
			}
			else {
				emit_mv_rd_rs1(dec);
				if (rs2x > 0) {
					as.imul(x86::gpq(rdx), x86::gpq(rs2x));
				} else {
					as.imul(x86::gpq(rdx), rbp_reg_q(dec.rs2));
				}
			}
			return true;
		}

		bool emit_mulh(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero || dec.rs2 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else {
				if (!MEMREG && rdx != 2 /* x86::rdx */) {
					as.mov(x86::qword_ptr(x86::rbp, proc_offset(ireg[rv_ireg_ra])), x86::rdx);
				}

				emit_mv_rax_rs1(dec);
				if (rs2x > 0) {
					as.imul(x86::gpq(rs2x));
				} else {
					as.mov(x86::rcx, rbp_reg_q(dec.rs2));
					as.imul(x86::rcx);
				}
				if (rdx > 0) {
					if (rdx != 2 /* x86::rdx */) {
						as.mov(x86::gpq(rdx), x86::rdx);
					}
				} else {
					as.mov(rbp_reg_q(dec.rd), x86::rdx);
				}

				if (!MEMREG && rdx != 2 /* x86::rdx */) {
					as.mov(x86::rdx, x86::qword_ptr(x86::rbp, proc_offset(ireg[rv_ireg_ra])));
				}
			}
			return true;
		}

		bool emit_mulhu(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero || dec.rs2 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else {
				if (!MEMREG && rdx != 2 /* x86::rdx */) {
					as.mov(x86::qword_ptr(x86::rbp, proc_offset(ireg[rv_ireg_ra])), x86::rdx);
				}

				emit_mv_rax_rs1(dec);
				if (rs2x > 0) {
					as.mul(x86::gpq(rs2x));
				} else {
					as.mov(x86::rcx, rbp_reg_q(dec.rs2));
					as.mul(x86::rcx);
				}
				if (rdx > 0) {
					if (rdx != 2 /* x86::rdx */) {
						as.mov(x86::gpq(rdx), x86::rdx);
					}
				} else {
					as.mov(rbp_reg_q(dec.rd), x86::rdx);
				}

				if (!MEMREG && rdx != 2 /* x86::rdx */) {
					as.mov(x86::rdx, x86::qword_ptr(x86::rbp, proc_offset(ireg[rv_ireg_ra])));
				}
			}
			return true;
		}

		bool emit_mulhsu(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero || dec.rs2 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else {
				if (!MEMREG && (rdx != 2 /* x86::rdx */ || (rs1x == 2 /* x86::rdx */ || rs2x == 2 /* x86::rdx */))) {
					as.mov(x86::qword_ptr(x86::rbp, proc_offset(ireg[rv_ireg_ra])), x86::rdx);
				}

				/* if rs1 is positive branch to umul */
				Label umul = as.newLabel();
				Label out = as.newLabel();
				emit_mv_rax_rs1(dec);
				as.test(x86::rax, x86::rax);
				as.jns(umul);
				as.neg(x86::rax);

				/* multiply, negate and stash result in rcx */
				if (rs2x > 0) {
					as.mul(x86::gpq(rs2x));
				} else {
					as.mov(x86::rcx, rbp_reg_q(dec.rs2));
					as.mul(x86::rcx);
				}
				as.xor_(x86::rdx, Imm(-1));
				as.mov(x86::rcx, x86::rdx);

				/* if necessary restore rdx input operand */
				if (!MEMREG && (rs1x == 2 || rs2x == 2 /* x86::rdx */)) {
					as.mov(x86::rdx, x86::qword_ptr(x86::rbp, proc_offset(ireg[rv_ireg_ra])));
				}

				/* second multiply */
				emit_mv_rax_rs1(dec);
				if (rs2x > 0) {
					as.imul(x86::rax, x86::gpq(rs2x));
				} else {
					as.imul(x86::rax, rbp_reg_q(dec.rs2));
				}

				/* handle zero special case */
				as.test(x86::rax, x86::rax);
				as.setz(x86::dl);
				as.movzx(x86::edx, x86::dl);
				as.lea(x86::rdx, x86::qword_ptr(x86::rdx, x86::rcx));
				as.jmp(out);

				/* unsigned multiply */
				as.bind(umul);
				if (rs2x > 0) {
					as.mul(x86::gpq(rs2x));
				} else {
					as.mov(x86::rcx, rbp_reg_q(dec.rs2));
					as.mul(x86::rcx);
				}

				/* high result */
				as.bind(out);
				if (rdx > 0) {
					if (rdx != 2 /* x86::rdx */) {
						as.mov(x86::gpq(rdx), x86::rdx);
					}
				} else {
					as.mov(rbp_reg_q(dec.rd), x86::rdx);
				}

				if (!MEMREG && (rdx != 2 /* x86::rdx */)) {
					as.mov(x86::rdx, x86::qword_ptr(x86::rbp, proc_offset(ireg[rv_ireg_ra])));
				}
			}
			return true;
		}

		bool emit_div(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs2 == rv_ireg_zero) {
				if (rdx > 0) {
					as.mov(x86::gpq(rdx), Imm(-1));
				} else {
					as.mov(rbp_reg_q(dec.rd), Imm(-1));
				}
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else {
				Label out = as.newLabel();
				Label div1 = as.newLabel();
				Label div2 = as.newLabel();

				as.mov(x86::rax, std::numeric_limits<int64_t>::min());
				if (rs1x > 0) {
					as.cmp(x86::gpq(rs1x), x86::rax);
				} else {
					as.cmp(rbp_reg_q(dec.rs1), x86::rax);
				}
				as.jne(div1);

				if (rs2x > 0) {
					as.cmp(x86::gpq(rs2x), Imm(-1));
				} else {
					as.cmp(rbp_reg_q(dec.rs2), Imm(-1));
				}
				as.je(out);

				as.bind(div1);
				if (rs2x > 0) {
					as.test(x86::gpq(rs2x), x86::gpq(rs2x));
				} else {
					as.cmp(rbp_reg_q(dec.rs2), Imm(0));
				}
				as.jne(div2);
				as.mov(x86::rax, Imm(-1));
				as.jmp(out);

				as.bind(div2);
				emit_mv_rax_rs1(dec);

				if (rdx != 2 /* rdx */) {
					as.mov(x86::rcx, x86::rdx);
				}
				as.cqo();
				if (rs2x > 0) {
					as.idiv(x86::gpq(rs2x));
				} else {
					as.idiv(rbp_reg_q(dec.rs2));
				}
				if (rdx != 2 /* rdx */) {
					as.mov(x86::rdx, x86::rcx);
				}

				as.bind(out);
				if (rdx > 0) {
					as.mov(x86::gpq(rdx), x86::rax);
				} else {
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}
			return true;
		}

		bool emit_rem(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs2 == rv_ireg_zero) {
				if (rdx > 0) {
					as.mov(x86::gpq(rdx), Imm(-1));
				} else {
					as.mov(rbp_reg_q(dec.rd), Imm(-1));
				}
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else {
				if (rdx != 2 /* rdx */) {
					as.mov(x86::rcx, x86::rdx);
				}

				Label out = as.newLabel();
				Label div1 = as.newLabel();
				Label div2 = as.newLabel();

				as.mov(x86::rax, std::numeric_limits<int64_t>::min());
				if (rs1x > 0) {
					as.cmp(x86::gpq(rs1x), x86::rax);
				} else {
					as.cmp(rbp_reg_q(dec.rs1), x86::rax);
				}
				as.jne(div1);

				as.xor_(x86::edx, x86::edx);
				if (rs2x > 0) {
					as.cmp(x86::gpq(rs2x), Imm(-1));
				} else {
					as.cmp(rbp_reg_q(dec.rs2), Imm(-1));
				}
				as.je(out);

				as.bind(div1);
				emit_mv_rax_rs1(dec);

				if (rs2x > 0) {
					as.test(x86::gpq(rs2x), x86::gpq(rs2x));
				} else {
					as.cmp(rbp_reg_q(dec.rs2), Imm(0));
				}
				as.jne(div2);
				as.mov(x86::rdx, x86::rax);
				as.jmp(out);

				as.bind(div2);
				as.cqo();
				if (rs2x > 0) {
					as.idiv(x86::gpq(rs2x));
				} else {
					as.idiv(rbp_reg_q(dec.rs2));
				}

				as.bind(out);
				if (rdx > 0) {
					if (rdx != 2 /* rdx */) {
						as.mov(x86::gpq(rdx), x86::rdx);
					}
				} else {
					as.mov(rbp_reg_q(dec.rd), x86::rdx);
				}

				if (rdx != 2 /* rdx */) {
					as.mov(x86::rdx, x86::rcx);
				}
			}
			return true;
		}

		bool emit_divu(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs2 == rv_ireg_zero) {
				if (rdx > 0) {
					as.mov(x86::gpq(rdx), Imm(-1));
				} else {
					as.mov(rbp_reg_q(dec.rd), Imm(-1));
				}
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else {
				Label out = as.newLabel();
				Label div1 = as.newLabel();

				if (rs2x > 0) {
					as.test(x86::gpq(rs2x), x86::gpq(rs2x));
				} else {
					as.cmp(rbp_reg_q(dec.rs2), Imm(0));
				}
				as.jne(div1);
				as.mov(x86::rax, Imm(-1));
				as.jmp(out);

				as.bind(div1);
				emit_mv_rax_rs1(dec);

				if (rdx != 2 /* rdx */) {
					as.mov(x86::rcx, x86::rdx);
				}
				as.xor_(x86::rdx, x86::rdx);
				if (rs2x > 0) {
					as.div(x86::gpq(rs2x));
				} else {
					as.div(rbp_reg_q(dec.rs2));
				}
				if (rdx != 2 /* rdx */) {
					as.mov(x86::rdx, x86::rcx);
				}

				as.bind(out);
				if (rdx > 0) {
					as.mov(x86::gpq(rdx), x86::rax);
				} else {
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}
			return true;
		}

		bool emit_remu(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs2 == rv_ireg_zero) {
				if (rdx > 0) {
					as.mov(x86::gpq(rdx), Imm(-1));
				} else {
					as.mov(rbp_reg_q(dec.rd), Imm(-1));
				}
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else {
				if (rdx != 2 /* rdx */) {
					as.mov(x86::rcx, x86::rdx);
				}

				Label out = as.newLabel();
				Label div1 = as.newLabel();

				emit_mv_rax_rs1(dec);
				if (rs2x > 0) {
					as.test(x86::gpq(rs2x), x86::gpq(rs2x));
				} else {
					as.cmp(rbp_reg_q(dec.rs2), Imm(0));
				}
				as.jne(div1);
				as.mov(x86::rdx, x86::rax);
				as.jmp(out);

				as.bind(div1);
				as.xor_(x86::rdx, x86::rdx);
				if (rs2x > 0) {
					as.div(x86::gpq(rs2x));
				} else {
					as.div(rbp_reg_q(dec.rs2));
				}

				as.bind(out);
				if (rdx > 0) {
					if (rdx != 2 /* rdx */) {
						as.mov(x86::gpq(rdx), x86::rdx);
					}
				} else {
					as.mov(rbp_reg_q(dec.rd), x86::rdx);
				}

				if (rdx != 2 /* rdx */) {
					as.mov(x86::rdx, x86::rcx);
				}
			}
			return true;
		}

		bool emit_slt(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else {
				if (rs1x > 0 && rs2x > 0) {
					as.cmp(x86::gpq(rs1x), x86::gpq(rs2x));
				}
				else {
					if (dec.rs1 == rv_ireg_zero) {
						as.xor_(x86::eax, x86::eax);
					} else if (rs1x > 0) {
						as.mov(x86::rax, x86::gpq(rs1x));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
					}
					if (dec.rs2 == rv_ireg_zero) {
						as.cmp(x86::rax, Imm(0));
					} else if (rs2x > 0) {
						as.cmp(x86::rax, x86::gpq(rs2x));
					} else {
						as.cmp(x86::rax, rbp_reg_q(dec.rs2));
					}
				}
				as.setl(x86::al);
				if (rdx > 0) {
					as.movzx(x86::gpd(rdx), x86::al);
				} else {
					as.movzx(x86::eax, x86::al);
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}
			return true;
		}

		bool emit_sltu(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else {
				if (rs1x > 0 && rs2x > 0) {
					as.cmp(x86::gpq(rs1x), x86::gpq(rs2x));
				}
				else {
					if (dec.rs1 == rv_ireg_zero) {
						as.xor_(x86::eax, x86::eax);
					} else if (rs1x > 0) {
						as.mov(x86::rax, x86::gpq(rs1x));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
					}
					if (dec.rs2 == rv_ireg_zero) {
						as.cmp(x86::rax, Imm(0));
					} else if (rs2x > 0) {
						as.cmp(x86::rax, x86::gpq(rs2x));
					} else {
						as.cmp(x86::rax, rbp_reg_q(dec.rs2));
					}
				}
				as.setb(x86::al);
				if (rdx > 0) {
					as.movzx(x86::gpd(rdx), x86::al);
				} else {
					as.movzx(x86::eax, x86::al);
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}
			return true;
		}

		bool emit_and(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero || dec.rs2 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else if (dec.rd == dec.rs1) {
				if (rdx > 0) {
					if (rs2x > 0) {
						as.and_(x86::gpq(rdx), x86::gpq(rs2x));
					} else {
						as.and_(x86::gpq(rdx), rbp_reg_q(dec.rs2));
					}
				} else {
					if (rs2x > 0) {
						as.and_(rbp_reg_q(dec.rd), x86::gpq(rs2x));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs2));
						as.and_(rbp_reg_q(dec.rd), x86::rax);
					}
				}
			}
			else if (dec.rd == dec.rs2) {
				if (rdx > 0) {
					if (rs1x > 0) {
						as.and_(x86::gpq(rdx), x86::gpq(rs1x));
					} else {
						as.and_(x86::gpq(rdx), rbp_reg_q(dec.rs1));
					}
				} else {
					if (rs1x > 0) {
						as.and_(rbp_reg_q(dec.rd), x86::gpq(rs1x));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.and_(rbp_reg_q(dec.rd), x86::rax);
					}
				}
			}
			else if (rdx < 0 && (rs1x < 0 || rs2x < 0)) {
				emit_mv_rax_rs1(dec);
				if (rs2x > 0) {
					as.and_(x86::rax, x86::gpq(rs2x));
				} else {
					as.and_(x86::rax, rbp_reg_q(dec.rs2));
				}
				as.mov(rbp_reg_q(dec.rd), x86::rax);
			}
			else {
				emit_mv_rd_rs1(dec);
				if (rdx > 0) {
					if (rs2x > 0) {
						as.and_(x86::gpq(rdx), x86::gpq(rs2x));
					} else {
						as.and_(x86::gpq(rdx), rbp_reg_q(dec.rs2));
					}
				} else {
					as.and_(rbp_reg_q(dec.rd), x86::gpq(rs2x));
				}
			}
			return true;
		}

		bool emit_or(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero && dec.rs2 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_mv_rd_rs2(dec);
			}
			else if (dec.rs2 == rv_ireg_zero) {
				emit_mv_rd_rs1(dec);
			}
			else if (dec.rd == dec.rs1) {
				if (rdx > 0) {
					if (rs2x > 0) {
						as.or_(x86::gpq(rdx), x86::gpq(rs2x));
					} else {
						as.or_(x86::gpq(rdx), rbp_reg_q(dec.rs2));
					}
				} else {
					if (rs2x > 0) {
						as.or_(rbp_reg_q(dec.rd), x86::gpq(rs2x));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs2));
						as.or_(rbp_reg_q(dec.rd), x86::rax);
					}
				}
			}
			else if (dec.rd == dec.rs2) {
				if (rdx > 0) {
					if (rs1x > 0) {
						as.or_(x86::gpq(rdx), x86::gpq(rs1x));
					} else {
						as.or_(x86::gpq(rdx), rbp_reg_q(dec.rs1));
					}
				} else {
					if (rs1x > 0) {
						as.or_(rbp_reg_q(dec.rd), x86::gpq(rs1x));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.or_(rbp_reg_q(dec.rd), x86::rax);
					}
				}
			}
			else if (rdx < 0 && (rs1x < 0 || rs2x < 0)) {
				emit_mv_rax_rs1(dec);
				if (rs2x > 0) {
					as.or_(x86::rax, x86::gpq(rs2x));
				} else {
					as.or_(x86::rax, rbp_reg_q(dec.rs2));
				}
				as.mov(rbp_reg_q(dec.rd), x86::rax);
			}
			else {
				emit_mv_rd_rs1(dec);
				if (rdx > 0) {
					if (rs2x > 0) {
						as.or_(x86::gpq(rdx), x86::gpq(rs2x));
					} else {
						as.or_(x86::gpq(rdx), rbp_reg_q(dec.rs2));
					}
				} else {
					as.or_(rbp_reg_q(dec.rd), x86::gpq(rs2x));
				}
			}
			return true;
		}

		bool emit_xor(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero && dec.rs2 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_mv_rd_rs2(dec);
			}
			else if (dec.rs2 == rv_ireg_zero) {
				emit_mv_rd_rs1(dec);
			}
			else if (dec.rd == dec.rs1) {
				if (rdx > 0) {
					if (rs2x > 0) {
						as.xor_(x86::gpq(rdx), x86::gpq(rs2x));
					} else {
						as.xor_(x86::gpq(rdx), rbp_reg_q(dec.rs2));
					}
				} else {
					if (rs2x > 0) {
						as.xor_(rbp_reg_q(dec.rd), x86::gpq(rs2x));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs2));
						as.xor_(rbp_reg_q(dec.rd), x86::rax);
					}
				}
			}
			else if (dec.rd == dec.rs2) {
				if (rdx > 0) {
					if (rs1x > 0) {
						as.xor_(x86::gpq(rdx), x86::gpq(rs1x));
					} else {
						as.xor_(x86::gpq(rdx), rbp_reg_q(dec.rs1));
					}
				} else {
					if (rs1x > 0) {
						as.xor_(rbp_reg_q(dec.rd), x86::gpq(rs1x));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.xor_(rbp_reg_q(dec.rd), x86::rax);
					}
				}
			}
			else if (rdx < 0 && (rs1x < 0 || rs2x < 0)) {
				emit_mv_rax_rs1(dec);
				if (rs2x > 0) {
					as.xor_(x86::rax, x86::gpq(rs2x));
				} else {
					as.xor_(x86::rax, rbp_reg_q(dec.rs2));
				}
				as.mov(rbp_reg_q(dec.rd), x86::rax);
			}
			else {
				emit_mv_rd_rs1(dec);
				if (rdx > 0) {
					if (rs2x > 0) {
						as.xor_(x86::gpq(rdx), x86::gpq(rs2x));
					} else {
						as.xor_(x86::gpq(rdx), rbp_reg_q(dec.rs2));
					}
				} else {
					as.xor_(rbp_reg_q(dec.rd), x86::gpq(rs2x));
				}
			}
			return true;
		}

		bool emit_sll(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else if (dec.rs2 == 0) {
				emit_mv_rd_rs1(dec);
			}
			else if (dec.rd == dec.rs1) {
				emit_mv_cl_rs2(dec);
				if (rdx > 0) {
					as.shl(x86::gpq(rdx), x86::cl);
				} else {
					as.shl(rbp_reg_q(dec.rd), x86::cl);
				}
			}
			else if (rdx < 0) {
				emit_mv_cl_rs2(dec);
				emit_mv_rax_rs1(dec);
				if (rs2x > 0) {
					as.shl(x86::rax, x86::cl);
				} else {
					as.shl(x86::rax, x86::cl);
				}
				as.mov(rbp_reg_q(dec.rd), x86::rax);
			}
			else {
				emit_mv_cl_rs2(dec);
				emit_mv_rd_rs1(dec);
				if (rs2x > 0) {
					as.shl(x86::gpq(rdx), x86::cl);
				} else {
					as.shl(x86::gpq(rdx), x86::cl);
				}
			}
			return true;
		}

		bool emit_srl(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else if (dec.rs2 == 0) {
				emit_mv_rd_rs1(dec);
			}
			else if (dec.rd == dec.rs1) {
				emit_mv_cl_rs2(dec);
				if (rdx > 0) {
					as.shr(x86::gpq(rdx), x86::cl);
				} else {
					as.shr(rbp_reg_q(dec.rd), x86::cl);
				}
			}
			else if (rdx < 0) {
				emit_mv_cl_rs2(dec);
				emit_mv_rax_rs1(dec);
				if (rs2x > 0) {
					as.shr(x86::rax, x86::cl);
				} else {
					as.shr(x86::rax, x86::cl);
				}
				as.mov(rbp_reg_q(dec.rd), x86::rax);
			}
			else {
				emit_mv_cl_rs2(dec);
				emit_mv_rd_rs1(dec);
				if (rs2x > 0) {
					as.shr(x86::gpq(rdx), x86::cl);
				} else {
					as.shr(x86::gpq(rdx), x86::cl);
				}
			}
			return true;
		}

		bool emit_sra(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else if (dec.rs2 == 0) {
				emit_mv_rd_rs1(dec);
			}
			else if (dec.rd == dec.rs1) {
				emit_mv_cl_rs2(dec);
				if (rdx > 0) {
					as.sar(x86::gpq(rdx), x86::cl);
				} else {
					as.sar(rbp_reg_q(dec.rd), x86::cl);
				}
			}
			else if (rdx < 0) {
				emit_mv_cl_rs2(dec);
				emit_mv_rax_rs1(dec);
				if (rs2x > 0) {
					as.sar(x86::rax, x86::cl);
				} else {
					as.sar(x86::rax, x86::cl);
				}
				as.mov(rbp_reg_q(dec.rd), x86::rax);
			}
			else {
				emit_mv_cl_rs2(dec);
				emit_mv_rd_rs1(dec);
				if (rs2x > 0) {
					as.sar(x86::gpq(rdx), x86::cl);
				} else {
					as.sar(x86::gpq(rdx), x86::cl);
				}
			}
			return true;
		}

		bool emit_addw(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_mv_rd_rs2_sx_32(dec);
			}
			else if (dec.rs2 == rv_ireg_zero) {
				emit_mv_rd_rs1_sx_32(dec);
			}
			else if (dec.rd == dec.rs1) {
				if (rdx > 0) {
					if (rs2x > 0) {
						as.add(x86::gpd(rdx), x86::gpd(rs2x));
					} else {
						as.add(x86::gpd(rdx), rbp_reg_d(dec.rs2));
					}
				} else {
					if (rs2x > 0) {
						as.add(rbp_reg_d(dec.rd), x86::gpd(rs2x));
					} else {
						as.mov(x86::eax, rbp_reg_d(dec.rs2));
						as.add(rbp_reg_d(dec.rd), x86::eax);
					}
				}
				emit_sx_32_rd(dec);
			}
			else if (dec.rd == dec.rs2) {
				if (rdx > 0) {
					if (rs1x > 0) {
						as.add(x86::gpd(rdx), x86::gpd(rs1x));
					} else {
						as.add(x86::gpd(rdx), rbp_reg_d(dec.rs1));
					}
				} else {
					if (rs1x > 0) {
						as.add(rbp_reg_d(dec.rd), x86::gpd(rs1x));
					} else {
						as.mov(x86::eax, rbp_reg_d(dec.rs1));
						as.add(rbp_reg_d(dec.rd), x86::eax);
					}
				}
				emit_sx_32_rd(dec);
			}
			else if (rdx > 0 && rs1x > 0 && rs2x > 0) {
				as.lea(x86::gpd(rdx), x86::dword_ptr(x86::gpd(rs1x), x86::gpd(rs2x)));
				emit_sx_32_rd(dec);
			}
			else if (rdx < 0) {
				if (rs1x > 0) {
					as.mov(x86::eax, x86::gpd(rs1x));
				} else {
					as.mov(x86::eax, rbp_reg_d(dec.rs1));
				}
				if (rs2x > 0) {
					as.add(x86::eax, x86::gpd(rs2x));
				} else {
					as.add(x86::eax, rbp_reg_d(dec.rs2));
				}
				as.movsxd(x86::rax, x86::eax);
				as.mov(rbp_reg_q(dec.rd), x86::rax);
			}
			else {
				emit_mv_rd_rs1(dec);
				if (rs2x > 0) {
					as.add(x86::gpd(rdx), x86::gpd(rs2x));
				} else {
					as.add(x86::gpd(rdx), rbp_reg_d(dec.rs2));
				}
				emit_sx_32_rd(dec);
			}
			return true;
		}

		bool emit_subw(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero) {
				if (dec.rd != dec.rs2) {
					emit_mv_rd_rs2(dec);
				}
				if (rdx > 0) {
					as.neg(x86::gpd(rdx));
				} else {
					as.neg(rbp_reg_d(dec.rd));
				}
				emit_sx_32_rd(dec);
			}
			else if (dec.rs2 == rv_ireg_zero) {
				emit_mv_rd_rs1_sx_32(dec);
			}
			else if (dec.rd == dec.rs1) {
				if (rdx > 0) {
					if (rs2x > 0) {
						as.sub(x86::gpd(rdx), x86::gpd(rs2x));
					} else {
						as.sub(x86::gpd(rdx), rbp_reg_d(dec.rs2));
					}
				} else {
					if (rs2x > 0) {
						as.sub(rbp_reg_d(dec.rd), x86::gpd(rs2x));
					} else {
						as.mov(x86::eax, rbp_reg_d(dec.rs2));
						as.sub(rbp_reg_d(dec.rd), x86::eax);
					}
				}
				emit_sx_32_rd(dec);
			}
			else if (dec.rd == dec.rs2) {
				if (rdx > 0) {
					if (rs1x > 0) {
						as.sub(x86::gpd(rdx), x86::gpd(rs1x));
					} else {
						as.sub(x86::gpd(rdx), rbp_reg_d(dec.rs1));
					}
				} else {
					if (rs1x > 0) {
						as.sub(rbp_reg_d(dec.rd), x86::gpd(rs1x));
					} else {
						as.mov(x86::eax, rbp_reg_d(dec.rs1));
						as.sub(rbp_reg_d(dec.rd), x86::eax);
					}
				}
				if (rdx > 0) {
					as.neg(x86::gpd(rdx));
				} else {
					as.neg(rbp_reg_d(dec.rd));
				}
				emit_sx_32_rd(dec);
			}
			else if (rdx < 0) {
				if (rs1x > 0) {
					as.mov(x86::eax, x86::gpd(rs1x));
				} else {
					as.mov(x86::eax, rbp_reg_d(dec.rs1));
				}
				if (rs2x > 0) {
					as.sub(x86::eax, x86::gpd(rs2x));
				} else {
					as.sub(x86::eax, rbp_reg_d(dec.rs2));
				}
				as.movsxd(x86::rax, x86::eax);
				as.mov(rbp_reg_q(dec.rd), x86::rax);
			}
			else {
				emit_mv_rd_rs1(dec);
				if (rs2x > 0) {
					as.sub(x86::gpd(rdx), x86::gpd(rs2x));
				} else {
					as.sub(x86::gpd(rdx), rbp_reg_d(dec.rs2));
				}
				emit_sx_32_rd(dec);
			}
			return true;
		}

		bool emit_mulw(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero || dec.rs2 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else if (dec.rd == dec.rs1 && rdx > 0) {
				if (rs2x > 0) {
					as.imul(x86::gpd(rdx), x86::gpd(rs2x));
				} else {
					as.imul(x86::gpd(rdx), rbp_reg_d(dec.rs2));
				}
				emit_sx_32_rd(dec);
			}
			else if (dec.rd == dec.rs2 && rdx > 0) {
				if (rs1x > 0) {
					as.imul(x86::gpd(rdx), x86::gpd(rs1x));
				} else {
					as.imul(x86::gpd(rdx), rbp_reg_d(dec.rs1));
				}
				emit_sx_32_rd(dec);
			}
			else if (rdx < 0) {
				if (rs1x > 0) {
					as.mov(x86::eax, x86::gpd(rs1x));
				} else {
					as.mov(x86::eax, rbp_reg_d(dec.rs1));
				}
				if (rs2x > 0) {
					as.imul(x86::eax, x86::gpd(rs2x));
				} else {
					as.imul(x86::eax, rbp_reg_d(dec.rs2));
				}
				as.mov(rbp_reg_d(dec.rd), x86::eax);
				emit_sx_32_rd(dec);
			}
			else {
				emit_mv_rd_rs1(dec);
				if (rs2x > 0) {
					as.imul(x86::gpd(rdx), x86::gpd(rs2x));
				} else {
					as.imul(x86::gpd(rdx), rbp_reg_d(dec.rs2));
				}
				emit_sx_32_rd(dec);
			}
			return true;
		}

		bool emit_divw(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs2 == rv_ireg_zero) {
				if (rdx > 0) {
					as.mov(x86::gpq(rdx), Imm(-1));
				} else {
					as.mov(rbp_reg_q(dec.rd), Imm(-1));
				}
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else {
				Label out = as.newLabel();
				Label div1 = as.newLabel();
				Label div2 = as.newLabel();

				as.mov(x86::eax, std::numeric_limits<int32_t>::min());
				if (rs1x > 0) {
					as.cmp(x86::gpd(rs1x), x86::eax);
				} else {
					as.cmp(rbp_reg_d(dec.rs1), x86::eax);
				}
				as.jne(div1);

				if (rs2x > 0) {
					as.cmp(x86::gpd(rs2x), Imm(-1));
				} else {
					as.cmp(rbp_reg_d(dec.rs2), Imm(-1));
				}
				as.je(out);

				as.bind(div1);
				if (rs2x > 0) {
					as.test(x86::gpq(rs2x), x86::gpq(rs2x));
				} else {
					as.cmp(rbp_reg_d(dec.rs2), Imm(0));
				}
				as.jne(div2);
				as.mov(x86::eax, Imm(-1));
				as.jmp(out);

				as.bind(div2);
				if (rs1x > 0) {
					as.mov(x86::eax, x86::gpd(rs1x));
				} else {
					as.mov(x86::eax, rbp_reg_d(dec.rs1));
				}

				if (rdx != 2 /* rdx */) {
					as.mov(x86::rcx, x86::rdx);
				}
				as.cdq();
				if (rs2x > 0) {
					as.idiv(x86::gpd(rs2x));
				} else {
					as.idiv(rbp_reg_d(dec.rs2));
				}
				if (rdx != 2 /* rdx */) {
					as.mov(x86::rdx, x86::rcx);
				}

				as.bind(out);
				if (rdx > 0) {
					as.movsxd(x86::gpq(rdx), x86::eax);
				} else {
					as.movsxd(x86::rax, x86::eax);
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}
			return true;
		}

		bool emit_remw(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs2 == rv_ireg_zero) {
				if (rdx > 0) {
					as.mov(x86::gpq(rdx), Imm(-1));
				} else {
					as.mov(rbp_reg_q(dec.rd), Imm(-1));
				}
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else {
				if (rdx != 2 /* rdx */) {
					as.mov(x86::rcx, x86::rdx);
				}

				Label out = as.newLabel();
				Label div1 = as.newLabel();
				Label div2 = as.newLabel();

				as.mov(x86::eax, std::numeric_limits<int32_t>::min());
				if (rs1x > 0) {
					as.cmp(x86::gpd(rs1x), x86::eax);
				} else {
					as.cmp(rbp_reg_d(dec.rs1), x86::eax);
				}
				as.jne(div1);

				as.xor_(x86::edx, x86::edx);
				if (rs2x > 0) {
					as.cmp(x86::gpd(rs2x), Imm(-1));
				} else {
					as.cmp(rbp_reg_d(dec.rs2), Imm(-1));
				}
				as.je(out);

				as.bind(div1);
				if (rs1x > 0) {
					as.mov(x86::eax, x86::gpd(rs1x));
				} else {
					as.mov(x86::eax, rbp_reg_d(dec.rs1));
				}

				if (rs2x > 0) {
					as.test(x86::gpq(rs2x), x86::gpq(rs2x));
				} else {
					as.cmp(rbp_reg_d(dec.rs2), Imm(0));
				}
				as.jne(div2);
				as.mov(x86::edx, x86::eax);
				as.jmp(out);

				as.bind(div2);
				as.cdq();
				if (rs2x > 0) {
					as.idiv(x86::gpd(rs2x));
				} else {
					as.idiv(rbp_reg_d(dec.rs2));
				}

				as.bind(out);
				if (rdx > 0) {
					as.movsxd(x86::gpq(rdx), x86::edx);
				} else {
					as.movsxd(x86::rdx, x86::edx);
					as.mov(rbp_reg_q(dec.rd), x86::rdx);
				}

				if (rdx != 2 /* rdx */) {
					as.mov(x86::rdx, x86::rcx);
				}
			}
			return true;
		}

		bool emit_divuw(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs2 == rv_ireg_zero) {
				if (rdx > 0) {
					as.mov(x86::gpq(rdx), Imm(-1));
				} else {
					as.mov(rbp_reg_q(dec.rd), Imm(-1));
				}
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else {
				Label out = as.newLabel();
				Label div1 = as.newLabel();

				if (rs2x > 0) {
					as.test(x86::gpq(rs2x), x86::gpq(rs2x));
				} else {
					as.cmp(rbp_reg_d(dec.rs2), Imm(0));
				}
				as.jne(div1);
				as.mov(x86::eax, Imm(-1));
				as.jmp(out);

				as.bind(div1);
				if (rs1x > 0) {
					as.mov(x86::eax, x86::gpd(rs1x));
				} else {
					as.mov(x86::eax, rbp_reg_d(dec.rs1));
				}

				if (rdx != 2 /* rdx */) {
					as.mov(x86::rcx, x86::rdx);
				}
				as.xor_(x86::edx, x86::edx);
				if (rs2x > 0) {
					as.div(x86::gpd(rs2x));
				} else {
					as.div(rbp_reg_d(dec.rs2));
				}
				if (rdx != 2 /* rdx */) {
					as.mov(x86::rdx, x86::rcx);
				}

				as.bind(out);
				if (rdx > 0) {
					as.movsxd(x86::gpq(rdx), x86::eax);
				} else {
					as.movsxd(x86::rax, x86::eax);
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}
			return true;
		}

		bool emit_remuw(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs2 == rv_ireg_zero) {
				if (rdx > 0) {
					as.mov(x86::gpq(rdx), Imm(-1));
				} else {
					as.mov(rbp_reg_q(dec.rd), Imm(-1));
				}
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else {
				if (rdx != 2 /* rdx */) {
					as.mov(x86::rcx, x86::rdx);
				}

				Label out = as.newLabel();
				Label div1 = as.newLabel();

				if (rs1x > 0) {
					as.mov(x86::eax, x86::gpd(rs1x));
				} else {
					as.mov(x86::eax, rbp_reg_d(dec.rs1));
				}

				if (rs2x > 0) {
					as.test(x86::gpq(rs2x), x86::gpq(rs2x));
				} else {
					as.cmp(rbp_reg_d(dec.rs2), Imm(0));
				}
				as.jne(div1);
				as.mov(x86::edx, x86::eax);
				as.jmp(out);

				as.bind(div1);
				as.xor_(x86::edx, x86::edx);
				if (rs2x > 0) {
					as.div(x86::gpd(rs2x));
				} else {
					as.div(rbp_reg_d(dec.rs2));
				}

				as.bind(out);
				if (rdx > 0) {
					as.movsxd(x86::gpq(rdx), x86::edx);
				} else {
					as.movsxd(x86::rdx, x86::edx);
					as.mov(rbp_reg_q(dec.rd), x86::rdx);
				}

				if (rdx != 2 /* rdx */) {
					as.mov(x86::rdx, x86::rcx);
				}
			}
			return true;
		}

		bool emit_sllw(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else if (dec.rs2 == 0) {
				emit_mv_rd_rs1_sx_32(dec);
			}
			else if (dec.rd == dec.rs1) {
				emit_mv_cl_rs2(dec);
				if (rdx > 0) {
					as.shl(x86::gpd(rdx), x86::cl);
				} else {
					as.shl(rbp_reg_d(dec.rd), x86::cl);
				}
				emit_sx_32_rd(dec);
			}
			else if (rdx < 0) {
				emit_mv_cl_rs2(dec);
				if (rs1x > 0) {
					as.mov(x86::eax, x86::gpd(rs1x));
				} else {
					as.mov(x86::eax, rbp_reg_d(dec.rs1));
				}
				if (rs2x > 0) {
					as.shl(x86::eax, x86::cl);
				} else {
					as.shl(x86::eax, x86::cl);
				}
				as.movsxd(x86::rax, x86::eax);
				as.mov(rbp_reg_q(dec.rd), x86::rax);
			}
			else {
				emit_mv_cl_rs2(dec);
				emit_mv_rd_rs1_32(dec);
				if (rs2x > 0) {
					as.shl(x86::gpd(rdx), x86::cl);
				} else {
					as.shl(x86::gpd(rdx), x86::cl);
				}
				emit_sx_32_rd(dec);
			}
			return true;
		}

		bool emit_srlw(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else if (dec.rs2 == 0) {
				emit_mv_rd_rs1_sx_32(dec);
			}
			else if (dec.rd == dec.rs1) {
				emit_mv_cl_rs2(dec);
				if (rdx > 0) {
					as.shr(x86::gpd(rdx), x86::cl);
				} else {
					as.shr(rbp_reg_d(dec.rd), x86::cl);
				}
				emit_sx_32_rd(dec);
			}
			else if (rdx < 0) {
				emit_mv_cl_rs2(dec);
				if (rs1x > 0) {
					as.mov(x86::eax, x86::gpd(rs1x));
				} else {
					as.mov(x86::eax, rbp_reg_d(dec.rs1));
				}
				if (rs2x > 0) {
					as.shr(x86::eax, x86::cl);
				} else {
					as.shr(x86::eax, x86::cl);
				}
				as.movsxd(x86::rax, x86::eax);
				as.mov(rbp_reg_q(dec.rd), x86::rax);
			}
			else {
				emit_mv_cl_rs2(dec);
				emit_mv_rd_rs1_32(dec);
				if (rs2x > 0) {
					as.shr(x86::gpd(rdx), x86::cl);
				} else {
					as.shr(x86::gpd(rdx), x86::cl);
				}
				emit_sx_32_rd(dec);
			}
			return true;
		}

		bool emit_sraw(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rs2x = x86_reg(dec.rs2);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else if (dec.rs2 == 0) {
				emit_mv_rd_rs1_sx_32(dec);
			}
			else if (dec.rd == dec.rs1) {
				emit_mv_cl_rs2(dec);
				if (rdx > 0) {
					as.sar(x86::gpd(rdx), x86::cl);
				} else {
					as.sar(rbp_reg_d(dec.rd), x86::cl);
				}
				emit_sx_32_rd(dec);
			}
			else if (rdx < 0) {
				emit_mv_cl_rs2(dec);
				if (rs1x > 0) {
					as.mov(x86::eax, x86::gpd(rs1x));
				} else {
					as.mov(x86::eax, rbp_reg_d(dec.rs1));
				}
				if (rs2x > 0) {
					as.sar(x86::eax, x86::cl);
				} else {
					as.sar(x86::eax, x86::cl);
				}
				as.movsxd(x86::rax, x86::eax);
				as.mov(rbp_reg_q(dec.rd), x86::rax);
			}
			else {
				emit_mv_cl_rs2(dec);
				emit_mv_rd_rs1_32(dec);
				if (rs2x > 0) {
					as.sar(x86::gpd(rdx), x86::cl);
				} else {
					as.sar(x86::gpd(rdx), x86::cl);
				}
				emit_sx_32_rd(dec);
			}
			return true;
		}

		bool emit_addi(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero) {
				if (rdx > 0) {
					as.mov(x86::gpq(rdx), Imm(dec.imm));
				} else {
					as.mov(rbp_reg_q(dec.rd), Imm(dec.imm));
				}
			}
			else if (dec.rd == dec.rs1) {
				if (rdx > 0) {
					as.add(x86::gpq(rdx), Imm(dec.imm));
				} else {
					as.add(rbp_reg_q(dec.rd), Imm(dec.imm));
				}
			}
			else if (dec.imm == 0) {
				emit_mv_rd_rs1(dec);
			}
			else if (rdx > 0 && rs1x > 0) {
				as.lea(x86::gpq(rdx), x86::qword_ptr(x86::gpq(rs1x), int32_t(dec.imm)));
			}
			else {
				emit_mv_rd_rs1(dec);
				if (rdx > 0) {
					as.add(x86::gpq(rdx), Imm(dec.imm));
				} else {
					as.add(rbp_reg_q(dec.rd), Imm(dec.imm));
				}
			}
			return true;
		}

		bool emit_slti(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero) {
				bool result = 0LL < dec.imm;
				if (rdx > 0) {
					as.mov(x86::gpq(rdx), Imm(result));
				} else {
					as.mov(rbp_reg_q(dec.rd), Imm(result));
				}
			}
			else {
				if (rs1x > 0) {
					as.cmp(x86::gpq(rs1x), Imm(dec.imm));
				} else {
					as.cmp(rbp_reg_q(dec.rs1), Imm(dec.imm));
				}
				as.setl(x86::al);
				if (rdx > 0) {
					as.movzx(x86::gpd(rdx), x86::al);
				} else {
					as.movzx(x86::eax, x86::al);
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}
			return true;
		}

		bool emit_sltiu(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero) {
				bool result = 0ULL < u64(dec.imm);
				if (rdx > 0) {
					as.mov(x86::gpq(rdx), Imm(result));
				} else {
					as.mov(rbp_reg_q(dec.rd), Imm(result));
				}
			}
			else {
				if (rs1x > 0) {
					as.cmp(x86::gpq(rs1x), Imm(dec.imm));
				} else {
					as.cmp(rbp_reg_q(dec.rs1), Imm(dec.imm));
				}
				as.setb(x86::al);
				if (rdx > 0) {
					as.movzx(x86::gpd(rdx), x86::al);
				} else {
					as.movzx(x86::eax, x86::al);
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}
			return true;
		}

		bool emit_andi(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else if (dec.rd == dec.rs1) {
				if (rdx > 0) {
					as.and_(x86::gpq(rdx), Imm(dec.imm));
				} else {
					as.and_(rbp_reg_q(dec.rd), Imm(dec.imm));
				}
			}
			else {
				emit_mv_rd_rs1(dec);
				if (rdx > 0) {
					as.and_(x86::gpq(rdx), Imm(dec.imm));
				} else {
					as.and_(rbp_reg_q(dec.rd), Imm(dec.imm));
				}
			}
			return true;
		}

		bool emit_ori(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero) {
				if (rdx > 0) {
					as.mov(x86::gpq(rdx), Imm(dec.imm));
				} else {
					as.mov(rbp_reg_q(dec.rd), Imm(dec.imm));
				}
			}
			else if (dec.rd == dec.rs1) {
				if (rdx > 0) {
					as.or_(x86::gpq(rdx), Imm(dec.imm));
				} else {
					as.or_(rbp_reg_q(dec.rd), Imm(dec.imm));
				}
			}
			else {
				emit_mv_rd_rs1(dec);
				if (rdx > 0) {
					as.or_(x86::gpq(rdx), Imm(dec.imm));
				} else {
					as.or_(rbp_reg_q(dec.rd), Imm(dec.imm));
				}
			}
			return true;
		}

		bool emit_xori(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero) {
				if (rdx > 0) {
					as.mov(x86::gpq(rdx), Imm(dec.imm));
				} else {
					as.mov(rbp_reg_q(dec.rd), Imm(dec.imm));
				}
			}
			else if (dec.rd == dec.rs1) {
				if (rdx > 0) {
					as.xor_(x86::gpq(rdx), Imm(dec.imm));
				} else {
					as.xor_(rbp_reg_q(dec.rd), Imm(dec.imm));
				}
			}
			else {
				emit_mv_rd_rs1(dec);
				if (rdx > 0) {
					as.xor_(x86::gpq(rdx), Imm(dec.imm));
				} else {
					as.xor_(rbp_reg_q(dec.rd), Imm(dec.imm));
				}
			}
			return true;
		}

		bool emit_slli(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else if (dec.rd == dec.rs1) {
				if (rdx > 0) {
					as.shl(x86::gpq(rdx), Imm(dec.imm));
				} else {
					as.shl(rbp_reg_q(dec.rd), Imm(dec.imm));
				}
			}
			else if (rdx < 0) {
				if (rs1x > 0) {
					as.mov(x86::rax, x86::gpq(rs1x));
				} else {
					as.mov(x86::rax, rbp_reg_q(dec.rs1));
				}
				as.shl(x86::rax, Imm(dec.imm));
				as.mov(rbp_reg_q(dec.rd), x86::rax);
			}
			else {
				emit_mv_rd_rs1(dec);
				if (rdx > 0) {
					as.shl(x86::gpq(rdx), Imm(dec.imm));
				} else {
					as.shl(rbp_reg_q(dec.rd), Imm(dec.imm));
				}
			}
			return true;
		}

		bool emit_srli(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else if (dec.rd == dec.rs1) {
				if (rdx > 0) {
					as.shr(x86::gpq(rdx), Imm(dec.imm));
				} else {
					as.shr(rbp_reg_q(dec.rd), Imm(dec.imm));
				}
			}
			else if (rdx < 0) {
				if (rs1x > 0) {
					as.mov(x86::rax, x86::gpq(rs1x));
				} else {
					as.mov(x86::rax, rbp_reg_q(dec.rs1));
				}
				as.shr(x86::rax, Imm(dec.imm));
				as.mov(rbp_reg_q(dec.rd), x86::rax);
			}
			else {
				emit_mv_rd_rs1(dec);
				if (rdx > 0) {
					as.shr(x86::gpq(rdx), Imm(dec.imm));
				} else {
					as.shr(rbp_reg_q(dec.rd), Imm(dec.imm));
				}
			}
			return true;
		}

		bool emit_srai(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else if (dec.rd == dec.rs1) {
				if (rdx > 0) {
					as.sar(x86::gpq(rdx), Imm(dec.imm));
				} else {
					as.sar(rbp_reg_q(dec.rd), Imm(dec.imm));
				}
			}
			else if (rdx < 0) {
				if (rs1x > 0) {
					as.mov(x86::rax, x86::gpq(rs1x));
				} else {
					as.mov(x86::rax, rbp_reg_q(dec.rs1));
				}
				as.sar(x86::rax, Imm(dec.imm));
				as.mov(rbp_reg_q(dec.rd), x86::rax);
			}
			else {
				emit_mv_rd_rs1(dec);
				if (rdx > 0) {
					as.sar(x86::gpq(rdx), Imm(dec.imm));
				} else {
					as.sar(rbp_reg_q(dec.rd), Imm(dec.imm));
				}
			}
			return true;
		}

		bool emit_addiw(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x= x86_reg(dec.rs1);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero) {
				if (rdx > 0) {
					as.mov(x86::gpd(rdx), Imm(dec.imm));
				} else {
					as.mov(rbp_reg_d(dec.rd), Imm(dec.imm));
				}
				emit_sx_32_rd(dec);
			}
			else if (dec.rd == dec.rs1) {
				if (rdx > 0) {
					as.add(x86::gpd(rdx), Imm(dec.imm));
				} else {
					as.add(rbp_reg_d(dec.rd), Imm(dec.imm));
				}
				emit_sx_32_rd(dec);
			}
			else if (rdx < 0) {
				if (rs1x > 0) {
					as.mov(x86::eax, x86::gpd(rs1x));
				} else {
					as.mov(x86::eax, rbp_reg_d(dec.rs1));
				}
				as.add(x86::eax, Imm(dec.imm));
				as.movsxd(x86::rax, x86::eax);
				as.mov(rbp_reg_q(dec.rd), x86::rax);
			}
			else {
				emit_mv_rd_rs1_32(dec);
				as.add(x86::gpd(rdx), Imm(dec.imm));
				emit_sx_32_rd(dec);
			}
			return true;
		}

		bool emit_slliw(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x= x86_reg(dec.rs1);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else if (dec.rd == dec.rs1) {
				if (rdx > 0) {
					as.shl(x86::gpd(rdx), Imm(dec.imm));
				} else {
					as.shl(rbp_reg_d(dec.rd), Imm(dec.imm));
				}
				emit_sx_32_rd(dec);
			}
			else if (rdx < 0) {
				if (rs1x > 0) {
					as.mov(x86::eax, x86::gpd(rs1x));
				} else {
					as.mov(x86::eax, rbp_reg_d(dec.rs1));
				}
				as.shl(x86::eax, Imm(dec.imm));
				as.movsxd(x86::rax, x86::eax);
				as.mov(rbp_reg_q(dec.rd), x86::rax);
			}
			else {
				emit_mv_rd_rs1_32(dec);
				as.shl(x86::gpd(rdx), Imm(dec.imm));
				emit_sx_32_rd(dec);
			}
			return true;
		}

		bool emit_srliw(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x= x86_reg(dec.rs1);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else if (dec.rd == dec.rs1) {
				if (rdx > 0) {
					as.shr(x86::gpd(rdx), Imm(dec.imm));
				} else {
					as.shr(rbp_reg_d(dec.rd), Imm(dec.imm));
				}
				emit_sx_32_rd(dec);
			}
			else if (rdx < 0) {
				if (rs1x > 0) {
					as.mov(x86::eax, x86::gpd(rs1x));
				} else {
					as.mov(x86::eax, rbp_reg_d(dec.rs1));
				}
				as.shr(x86::eax, Imm(dec.imm));
				as.movsxd(x86::rax, x86::eax);
				as.mov(rbp_reg_q(dec.rd), x86::rax);
			}
			else {
				emit_mv_rd_rs1_32(dec);
				as.shr(x86::gpd(rdx), Imm(dec.imm));
				emit_sx_32_rd(dec);
			}
			return true;
		}

		bool emit_sraiw(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x= x86_reg(dec.rs1);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else if (dec.rs1 == rv_ireg_zero) {
				emit_zero_rd(dec);
			}
			else if (dec.rd == dec.rs1) {
				if (rdx > 0) {
					as.sar(x86::gpd(rdx), Imm(dec.imm));
				} else {
					as.sar(rbp_reg_d(dec.rd), Imm(dec.imm));
				}
				emit_sx_32_rd(dec);
			}
			else if (rdx < 0) {
				if (rs1x > 0) {
					as.mov(x86::eax, x86::gpd(rs1x));
				} else {
					as.mov(x86::eax, rbp_reg_d(dec.rs1));
				}
				as.sar(x86::eax, Imm(dec.imm));
				as.movsxd(x86::rax, x86::eax);
				as.mov(rbp_reg_q(dec.rd), x86::rax);
			}
			else {
				emit_mv_rd_rs1_32(dec);
				as.sar(x86::gpd(rdx), Imm(dec.imm));
				emit_sx_32_rd(dec);
			}
			return true;
		}

		void emit_cmp(decode_type &dec)
		{
			int rs1x = x86_reg(dec.rs1), rs2x = x86_reg(dec.rs2);
			if (dec.rs1 == rv_ireg_zero) {
				as.xor_(x86::eax, x86::eax);
				if (rs2x > 0) {
					as.cmp(x86::rax, x86::gpq(rs2x));
				} else {
					as.cmp(x86::rax, rbp_reg_q(dec.rs2));
				}
			}
			else if (dec.rs2 == rv_ireg_zero) {
				if (rs1x > 0) {
					as.cmp(x86::gpq(rs1x), Imm(0));
				} else {
					as.cmp(rbp_reg_q(dec.rs1), Imm(0));
				}
			}
			else if (rs1x > 0) {
				if (rs2x > 0) {
					as.cmp(x86::gpq(rs1x), x86::gpq(rs2x));
				} else {
					as.cmp(x86::gpq(rs1x), rbp_reg_q(dec.rs2));
				}
			}
			else {
				if (rs2x > 0) {
					as.cmp(rbp_reg_q(dec.rs1), x86::gpq(rs2x));
				} else {
					as.mov(x86::rax, rbp_reg_q(dec.rs1));
					as.cmp(x86::rax, rbp_reg_q(dec.rs2));
				}
			}
		}

		inline auto create_exit_tramp(addr_t pc)
		{
			auto jtl = exit_tramp_labels.find(pc);
			if (jtl == exit_tramp_labels.end()) {
				jtl = exit_tramp_labels.insert(exit_tramp_labels.end(),
					std::pair<addr_t,Label>(pc, as.newLabel()));
			}
			return jtl;
		}

		inline auto create_jump_tramp(addr_t pc)
		{
			auto jtl = jmp_tramp_labels.find(pc);
			if (jtl == jmp_tramp_labels.end()) {
				jtl = jmp_tramp_labels.insert(jmp_tramp_labels.end(),
					std::pair<addr_t,Label>(pc, as.newLabel()));
			}
			return jtl;
		}

		inline auto create_jump_fixup(addr_t pc)
		{
			auto jfl = jmp_fixup_labels.find(pc);
			if (jfl == jmp_fixup_labels.end()) {
				jfl = jmp_fixup_labels.insert(jmp_fixup_labels.end(),
					std::pair<addr_t,std::vector<Label>>(pc, std::vector<Label>()));
			}
			return jfl;
		}

		void emit_jump_fixup(addr_t pc)
		{
			auto jtl = create_jump_tramp(pc);
			auto jfl = create_jump_fixup(pc);
			as.jmp(jtl->second);
			Label label = as.newLabel();
			as.bind(label);
			jfl->second.push_back(label);
		}

		void emit_branch_fixup(x86::Cond bf, addr_t pc)
		{
			auto jtl = create_jump_tramp(pc);
			auto jfl = create_jump_fixup(pc);
			as.j(bf, jtl->second);
			Label label = as.newLabel();
			as.bind(label);
			jfl->second.push_back(label);
		}

		bool emit_branch(decode_type &dec, bool cond, x86::Cond bf, x86::Cond ibf)
		{
			addr_t branch_pc = dec.pc + dec.imm;
			addr_t cont_pc = dec.pc + inst_length(dec.inst);
			auto branch_i = labels.find(branch_pc);
			auto cont_i = labels.find(cont_pc);

			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());

			commit_instret();

			emit_cmp(dec);

			if (branch_i != labels.end() && cont_i != labels.end()) {
				as.j(bf, branch_i->second);
				as.jmp(cont_i->second);
			}
			else if (cond && branch_i != labels.end()) {
				as.j(bf, branch_i->second);
				uintptr_t cont_addr = lookup_trace_slow(cont_pc);
				if (cont_addr) {
					as.jmp(Imm(cont_addr));
				} else {
					emit_jump_fixup(cont_pc);
				}
				term_pc = 0;
			}
			else if (!cond && cont_i != labels.end()) {
				as.j(ibf, cont_i->second);
				uintptr_t branch_addr = lookup_trace_slow(branch_pc);
				if (branch_addr) {
					as.jmp(Imm(branch_addr));
				} else {
					emit_jump_fixup(branch_pc);
				}
				term_pc = 0;
			} else if (cond) {
				uintptr_t cont_addr = lookup_trace_slow(cont_pc);
				if (cont_addr) {
					as.j(ibf, Imm(cont_addr));
				} else {
					emit_branch_fixup(ibf, cont_pc);
				}
				term_pc = branch_pc;
			} else {
				uintptr_t branch_addr = lookup_trace_slow(branch_pc);
				if (branch_addr) {
					as.j(bf, Imm(branch_addr));
				} else {
					emit_branch_fixup(bf, branch_pc);
				}
				term_pc = cont_pc;
			}
			return true;
		}

		bool emit_bne(decode_type &dec)
		{
			return emit_branch(dec, dec.brc, x86::kCondNE, x86::kCondE);
		}

		bool emit_beq(decode_type &dec)
		{
			return emit_branch(dec, dec.brc, x86::kCondE, x86::kCondNE);
		}

		bool emit_blt(decode_type &dec)
		{
			return emit_branch(dec, dec.brc, x86::kCondL, x86::kCondGE);
		}

		bool emit_bge(decode_type &dec)
		{
			return emit_branch(dec, dec.brc, x86::kCondGE, x86::kCondL);
		}

		bool emit_bltu(decode_type &dec)
		{
			return emit_branch(dec, dec.brc, x86::kCondB, x86::kCondAE);
		}

		bool emit_bgeu(decode_type &dec)
		{
			return emit_branch(dec, dec.brc, x86::kCondAE, x86::kCondB);
		}

		bool emit_ld(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else {
				if (use_mmu) {
					if (dec.rs1 == rv_ireg_zero) {
						as.mov(x86::rax, Imm(dec.imm));
					}
					else if (rs1x > 0) {
						as.lea(x86::rax, x86::qword_ptr(x86::gpq(rs1x), dec.imm));
					}
					else {
						as.mov(x86::rcx, rbp_reg_q(dec.rs1));
						as.lea(x86::rax, x86::qword_ptr(x86::rcx, dec.imm));
					}
					as.call(Imm(func_address(ops.ld)));
					auto okay = as.newLabel();
					as.cmp(x86::qword_ptr(x86::rbp, proc_offset(cause)), Imm(0));
					as.je(okay);
					emit_pc(dec.pc);
					as.jmp(term);
					as.bind(okay);
					if (rdx > 0) {
						as.mov(x86::gpq(rdx), x86::rax);
					} else {
						as.mov(rbp_reg_q(dec.rd), x86::rax);
					}
				}
				else if (rdx > 0) {
					if (rs1x > 0) {
						as.mov(x86::gpq(rdx), x86::qword_ptr(x86::gpq(rs1x), dec.imm));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.mov(x86::gpq(rdx), x86::qword_ptr(x86::rax, dec.imm));
					}
				}
				else {
					if (rs1x > 0) {
						as.mov(x86::rax, x86::qword_ptr(x86::gpq(rs1x), dec.imm));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.mov(x86::rax, x86::qword_ptr(x86::rax, dec.imm));
					}
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}
			return true;
		}

		bool emit_lw(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else {
				if (use_mmu) {
					if (dec.rs1 == rv_ireg_zero) {
						as.mov(x86::rax, Imm(dec.imm));
					}
					else if (rs1x > 0) {
						as.lea(x86::rax, x86::qword_ptr(x86::gpq(rs1x), dec.imm));
					}
					else {
						as.mov(x86::rcx, rbp_reg_q(dec.rs1));
						as.lea(x86::rax, x86::qword_ptr(x86::rcx, dec.imm));
					}
					as.call(Imm(func_address(ops.lw)));
					auto okay = as.newLabel();
					as.cmp(x86::qword_ptr(x86::rbp, proc_offset(cause)), Imm(0));
					as.je(okay);
					emit_pc(dec.pc);
					as.jmp(term);
					as.bind(okay);
					if (rdx > 0) {
						as.movsxd(x86::gpq(rdx), x86::eax);
					} else {
						as.movsxd(x86::rax, x86::eax);
						as.mov(rbp_reg_q(dec.rd), x86::rax);
					}
				}
				else if (rdx > 0) {
					if (rs1x > 0) {
						as.movsxd(x86::gpq(rdx), x86::dword_ptr(x86::gpq(rs1x), dec.imm));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.movsxd(x86::gpq(rdx), x86::dword_ptr(x86::rax, dec.imm));
					}
				}
				else {
					if (rs1x > 0) {
						as.movsxd(x86::rax, x86::dword_ptr(x86::gpq(rs1x), dec.imm));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.movsxd(x86::rax, x86::dword_ptr(x86::rax, dec.imm));
					}
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}
			return true;
		}

		bool emit_lwu(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else {
				if (use_mmu) {
					if (dec.rs1 == rv_ireg_zero) {
						as.mov(x86::rax, Imm(dec.imm));
					}
					else if (rs1x > 0) {
						as.lea(x86::rax, x86::qword_ptr(x86::gpq(rs1x), dec.imm));
					}
					else {
						as.mov(x86::rcx, rbp_reg_q(dec.rs1));
						as.lea(x86::rax, x86::qword_ptr(x86::rcx, dec.imm));
					}
					as.call(Imm(func_address(ops.lw)));
					auto okay = as.newLabel();
					as.cmp(x86::qword_ptr(x86::rbp, proc_offset(cause)), Imm(0));
					as.je(okay);
					emit_pc(dec.pc);
					as.jmp(term);
					as.bind(okay);
					if (rdx > 0) {
						as.mov(x86::gpd(rdx), x86::eax);
					} else {
						as.mov(rbp_reg_q(dec.rd), x86::rax);
					}
				}
				else if (rdx > 0) {
					if (rs1x > 0) {
						as.mov(x86::gpd(rdx), x86::dword_ptr(x86::gpq(rs1x), dec.imm));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.mov(x86::gpd(rdx), x86::dword_ptr(x86::rax, dec.imm));
					}
				}
				else {
					if (rs1x > 0) {
						as.mov(x86::eax, x86::dword_ptr(x86::gpq(rs1x), dec.imm));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.mov(x86::eax, x86::dword_ptr(x86::rax, dec.imm));
					}
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}
			return true;
		}

		bool emit_lh(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else {
				if (use_mmu) {
					if (dec.rs1 == rv_ireg_zero) {
						as.mov(x86::rax, Imm(dec.imm));
					}
					else if (rs1x > 0) {
						as.lea(x86::rax, x86::qword_ptr(x86::gpq(rs1x), dec.imm));
					}
					else {
						as.mov(x86::rcx, rbp_reg_q(dec.rs1));
						as.lea(x86::rax, x86::qword_ptr(x86::rcx, dec.imm));
					}
					as.call(Imm(func_address(ops.lh)));
					auto okay = as.newLabel();
					as.cmp(x86::qword_ptr(x86::rbp, proc_offset(cause)), Imm(0));
					as.je(okay);
					emit_pc(dec.pc);
					as.jmp(term);
					as.bind(okay);
					if (rdx > 0) {
						as.movsx(x86::gpq(rdx), x86::ax);
					} else {
						as.movsx(x86::rax, x86::ax);
						as.mov(rbp_reg_q(dec.rd), x86::rax);
					}
				}
				else if (rdx > 0) {
					if (rs1x > 0) {
						as.movsx(x86::gpq(rdx), x86::word_ptr(x86::gpq(rs1x), dec.imm));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.movsx(x86::gpq(rdx), x86::word_ptr(x86::rax, dec.imm));
					}
				}
				else {
					if (rs1x > 0) {
						as.movsx(x86::rax, x86::word_ptr(x86::gpq(rs1x), dec.imm));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.movsx(x86::rax, x86::word_ptr(x86::rax, dec.imm));
					}
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}
			return true;
		}

		bool emit_lhu(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else {
				if (use_mmu) {
					if (dec.rs1 == rv_ireg_zero) {
						as.mov(x86::rax, Imm(dec.imm));
					}
					else if (rs1x > 0) {
						as.lea(x86::rax, x86::qword_ptr(x86::gpq(rs1x), dec.imm));
					}
					else {
						as.mov(x86::rcx, rbp_reg_q(dec.rs1));
						as.lea(x86::rax, x86::qword_ptr(x86::rcx, dec.imm));
					}
					as.call(Imm(func_address(ops.lh)));
					auto okay = as.newLabel();
					as.cmp(x86::qword_ptr(x86::rbp, proc_offset(cause)), Imm(0));
					as.je(okay);
					emit_pc(dec.pc);
					as.jmp(term);
					as.bind(okay);
					if (rdx > 0) {
						as.mov(x86::gpd(rdx), x86::eax);
					} else {
						as.mov(rbp_reg_q(dec.rd), x86::rax);
					}
				}
				else if (rdx > 0) {
					if (rs1x > 0) {
						as.movzx(x86::gpd(rdx), x86::word_ptr(x86::gpq(rs1x), dec.imm));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.movzx(x86::gpd(rdx), x86::word_ptr(x86::rax, dec.imm));
					}
				}
				else {
					if (rs1x > 0) {
						as.movzx(x86::eax, x86::word_ptr(x86::gpq(rs1x), dec.imm));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.movzx(x86::eax, x86::word_ptr(x86::rax, dec.imm));
					}
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}
			return true;
		}

		bool emit_lb(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else {
				if (use_mmu) {
					if (dec.rs1 == rv_ireg_zero) {
						as.mov(x86::rax, Imm(dec.imm));
					}
					else if (rs1x > 0) {
						as.lea(x86::rax, x86::qword_ptr(x86::gpq(rs1x), dec.imm));
					}
					else {
						as.mov(x86::rcx, rbp_reg_q(dec.rs1));
						as.lea(x86::rax, x86::qword_ptr(x86::rcx, dec.imm));
					}
					as.call(Imm(func_address(ops.lb)));
					auto okay = as.newLabel();
					as.cmp(x86::qword_ptr(x86::rbp, proc_offset(cause)), Imm(0));
					as.je(okay);
					emit_pc(dec.pc);
					as.jmp(term);
					as.bind(okay);
					if (rdx > 0) {
						as.movsx(x86::gpq(rdx), x86::al);
					} else {
						as.movsx(x86::rax, x86::al);
						as.mov(rbp_reg_q(dec.rd), x86::rax);
					}
				}
				else if (rdx > 0) {
					if (rs1x > 0) {
						as.movsx(x86::gpq(rdx), x86::byte_ptr(x86::gpq(rs1x), dec.imm));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.movsx(x86::gpq(rdx), x86::byte_ptr(x86::rax, dec.imm));
					}
				}
				else {
					if (rs1x > 0) {
						as.movsx(x86::rax, x86::byte_ptr(x86::gpq(rs1x), dec.imm));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.movsx(x86::rax, x86::byte_ptr(x86::rax, dec.imm));
					}
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}
			return true;
		}

		bool emit_lbu(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else {
				if (use_mmu) {
					if (dec.rs1 == rv_ireg_zero) {
						as.mov(x86::rax, Imm(dec.imm));
					}
					else if (rs1x > 0) {
						as.lea(x86::rax, x86::qword_ptr(x86::gpq(rs1x), dec.imm));
					}
					else {
						as.mov(x86::rcx, rbp_reg_q(dec.rs1));
						as.lea(x86::rax, x86::qword_ptr(x86::rcx, dec.imm));
					}
					as.call(Imm(func_address(ops.lb)));
					auto okay = as.newLabel();
					as.cmp(x86::qword_ptr(x86::rbp, proc_offset(cause)), Imm(0));
					as.je(okay);
					emit_pc(dec.pc);
					as.jmp(term);
					as.bind(okay);
					if (rdx > 0) {
						as.mov(x86::gpd(rdx), x86::eax);
					} else {
						as.mov(rbp_reg_q(dec.rd), x86::rax);
					}
				}
				else if (rdx > 0) {
					if (rs1x > 0) {
						as.movzx(x86::gpd(rdx), x86::byte_ptr(x86::gpq(rs1x), dec.imm));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.movzx(x86::gpd(rdx), x86::byte_ptr(x86::rax, dec.imm));
					}
				}
				else {
					if (rs1x > 0) {
						as.movzx(x86::eax, x86::byte_ptr(x86::gpq(rs1x), dec.imm));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.movzx(x86::eax, x86::byte_ptr(x86::rax, dec.imm));
					}
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}
			return true;
		}

		bool emit_sd(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rs2x = x86_reg(dec.rs2), rs1x = x86_reg(dec.rs1);
			if (dec.rs2 == rv_ireg_zero) {
				if (use_mmu) {
					if (rs1x > 0) {
						as.lea(x86::rax, x86::qword_ptr(x86::gpq(rs1x), dec.imm));
					} else {
						as.mov(x86::rcx, rbp_reg_q(dec.rs1));
						as.lea(x86::rax, x86::qword_ptr(x86::rcx, dec.imm));
					}
					as.xor_(x86::ecx, x86::ecx);
					as.call(Imm(func_address(ops.sd)));
					auto okay = as.newLabel();
					as.cmp(x86::qword_ptr(x86::rbp, proc_offset(cause)), Imm(0));
					as.je(okay);
					emit_pc(dec.pc);
					as.jmp(term);
					as.bind(okay);
				}
				else if (rs1x > 0) {
					as.mov(x86::qword_ptr(x86::gpq(rs1x), dec.imm), Imm(0));
				} else {
					as.mov(x86::rax, rbp_reg_q(dec.rs1));
					as.mov(x86::qword_ptr(x86::rax, dec.imm), Imm(0));
				}
			}
			else {
				if (use_mmu) {
					if (rs1x > 0) {
						as.lea(x86::rax, x86::qword_ptr(x86::gpq(rs1x), dec.imm));
					} else {
						as.mov(x86::rcx, rbp_reg_q(dec.rs1));
						as.lea(x86::rax, x86::qword_ptr(x86::rcx, dec.imm));
					}
					if (rs2x > 0) {
						as.mov(x86::rcx, x86::gpq(rs2x));
					} else {
						as.mov(x86::rcx, rbp_reg_q(dec.rs2));
					}
					as.call(Imm(func_address(ops.sd)));
					auto okay = as.newLabel();
					as.cmp(x86::qword_ptr(x86::rbp, proc_offset(cause)), Imm(0));
					as.je(okay);
					emit_pc(dec.pc);
					as.jmp(term);
					as.bind(okay);
				}
				else if (rs2x > 0) {
					if (rs1x > 0) {
						as.mov(x86::qword_ptr(x86::gpq(rs1x), dec.imm), x86::gpq(rs2x));
					}
					else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.mov(x86::qword_ptr(x86::rax, dec.imm), x86::gpq(rs2x));
					}
				}
				else {
					as.mov(x86::rcx, rbp_reg_q(dec.rs2));
					if (rs1x > 0) {
						as.mov(x86::qword_ptr(x86::gpq(rs1x), dec.imm), x86::rcx);
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.mov(x86::qword_ptr(x86::rax, dec.imm), x86::rcx);
					}
				}
			}
			return true;
		}

		bool emit_sw(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rs2x = x86_reg(dec.rs2), rs1x = x86_reg(dec.rs1);
			if (dec.rs2 == rv_ireg_zero) {
				if (use_mmu) {
					if (rs1x > 0) {
						as.lea(x86::rax, x86::qword_ptr(x86::gpq(rs1x), dec.imm));
					} else {
						as.mov(x86::rcx, rbp_reg_q(dec.rs1));
						as.lea(x86::rax, x86::qword_ptr(x86::rcx, dec.imm));
					}
					as.xor_(x86::ecx, x86::ecx);
					as.call(Imm(func_address(ops.sw)));
					auto okay = as.newLabel();
					as.cmp(x86::qword_ptr(x86::rbp, proc_offset(cause)), Imm(0));
					as.je(okay);
					emit_pc(dec.pc);
					as.jmp(term);
					as.bind(okay);
				}
				else if (rs1x > 0) {
					as.mov(x86::dword_ptr(x86::gpq(rs1x), dec.imm), Imm(0));
				} else {
					as.mov(x86::rax, rbp_reg_q(dec.rs1));
					as.mov(x86::dword_ptr(x86::rax, dec.imm), Imm(0));
				}
			}
			else {
				if (use_mmu) {
					if (rs1x > 0) {
						as.lea(x86::rax, x86::qword_ptr(x86::gpq(rs1x), dec.imm));
					} else {
						as.mov(x86::rcx, rbp_reg_q(dec.rs1));
						as.lea(x86::rax, x86::qword_ptr(x86::rcx, dec.imm));
					}
					if (rs2x > 0) {
						as.mov(x86::ecx, x86::gpd(rs2x));
					} else {
						as.mov(x86::ecx, rbp_reg_d(dec.rs2));
					}
					as.call(Imm(func_address(ops.sw)));
					auto okay = as.newLabel();
					as.cmp(x86::qword_ptr(x86::rbp, proc_offset(cause)), Imm(0));
					as.je(okay);
					emit_pc(dec.pc);
					as.jmp(term);
					as.bind(okay);
				}
				else if (rs2x > 0) {
					if (rs1x > 0) {
						as.mov(x86::dword_ptr(x86::gpq(rs1x), dec.imm), x86::gpd(rs2x));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.mov(x86::dword_ptr(x86::rax, dec.imm), x86::gpd(rs2x));
					}
				}
				else {
					as.mov(x86::rcx, rbp_reg_q(dec.rs2));
					if (rs1x > 0) {
						as.mov(x86::dword_ptr(x86::gpq(rs1x), dec.imm), x86::ecx);
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.mov(x86::dword_ptr(x86::rax, dec.imm), x86::ecx);
					}
				}
			}
			return true;
		}

		bool emit_sh(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rs2x = x86_reg(dec.rs2), rs1x = x86_reg(dec.rs1);
			if (dec.rs2 == rv_ireg_zero) {
				if (use_mmu) {
					if (rs1x > 0) {
						as.lea(x86::rax, x86::qword_ptr(x86::gpq(rs1x), dec.imm));
					} else {
						as.mov(x86::rcx, rbp_reg_q(dec.rs1));
						as.lea(x86::rax, x86::qword_ptr(x86::rcx, dec.imm));
					}
					as.xor_(x86::ecx, x86::ecx);
					as.call(Imm(func_address(ops.sh)));
					auto okay = as.newLabel();
					as.cmp(x86::qword_ptr(x86::rbp, proc_offset(cause)), Imm(0));
					as.je(okay);
					emit_pc(dec.pc);
					as.jmp(term);
					as.bind(okay);
				}
				else if (rs1x > 0) {
					as.mov(x86::word_ptr(x86::gpq(rs1x), dec.imm), Imm(0));
				}
				else {
					as.mov(x86::rax, rbp_reg_q(dec.rs1));
					as.mov(x86::word_ptr(x86::rax, dec.imm), Imm(0));
				}
			}
			else {
				if (use_mmu) {
					if (rs1x > 0) {
						as.lea(x86::rax, x86::qword_ptr(x86::gpq(rs1x), dec.imm));
					} else {
						as.mov(x86::rcx, rbp_reg_q(dec.rs1));
						as.lea(x86::rax, x86::qword_ptr(x86::rcx, dec.imm));
					}
					if (rs2x > 0) {
						as.mov(x86::ecx, x86::gpd(rs2x));
					} else {
						as.mov(x86::ecx, rbp_reg_d(dec.rs2));
					}
					as.call(Imm(func_address(ops.sh)));
					auto okay = as.newLabel();
					as.cmp(x86::qword_ptr(x86::rbp, proc_offset(cause)), Imm(0));
					as.je(okay);
					emit_pc(dec.pc);
					as.jmp(term);
					as.bind(okay);
				}
				else if (rs2x > 0) {
					if (rs1x > 0) {
						as.mov(x86::word_ptr(x86::gpq(rs1x), dec.imm), x86::gpw(rs2x));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.mov(x86::word_ptr(x86::rax, dec.imm), x86::gpw(rs2x));
					}
				}
				else {
					as.mov(x86::rcx, rbp_reg_q(dec.rs2));
					if (rs1x > 0) {
						as.mov(x86::word_ptr(x86::gpq(rs1x), dec.imm), x86::cx);
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.mov(x86::word_ptr(x86::rax, dec.imm), x86::cx);
					}
				}
			}
			return true;
		}

		bool emit_sb(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rs2x = x86_reg(dec.rs2), rs1x = x86_reg(dec.rs1);
			if (dec.rs2 == rv_ireg_zero) {
				if (use_mmu) {
					if (rs1x > 0) {
						as.lea(x86::rax, x86::qword_ptr(x86::gpq(rs1x), dec.imm));
					} else {
						as.mov(x86::rcx, rbp_reg_q(dec.rs1));
						as.lea(x86::rax, x86::qword_ptr(x86::rcx, dec.imm));
					}
					as.xor_(x86::ecx, x86::ecx);
					as.call(Imm(func_address(ops.sb)));
					auto okay = as.newLabel();
					as.cmp(x86::qword_ptr(x86::rbp, proc_offset(cause)), Imm(0));
					as.je(okay);
					emit_pc(dec.pc);
					as.jmp(term);
					as.bind(okay);
				}
				else if (rs1x > 0) {
					as.mov(x86::byte_ptr(x86::gpq(rs1x), dec.imm), Imm(0));
				}
				else {
					as.mov(x86::rax, rbp_reg_q(dec.rs1));
					as.mov(x86::byte_ptr(x86::rax, dec.imm), Imm(0));
				}
			}
			else {
				if (use_mmu) {
					if (rs1x > 0) {
						as.lea(x86::rax, x86::qword_ptr(x86::gpq(rs1x), dec.imm));
					} else {
						as.mov(x86::rcx, rbp_reg_q(dec.rs1));
						as.lea(x86::rax, x86::qword_ptr(x86::rcx, dec.imm));
					}
					if (rs2x > 0) {
						as.mov(x86::ecx, x86::gpd(rs2x));
					} else {
						as.mov(x86::ecx, rbp_reg_d(dec.rs2));
					}
					as.call(Imm(func_address(ops.sb)));
					auto okay = as.newLabel();
					as.cmp(x86::qword_ptr(x86::rbp, proc_offset(cause)), Imm(0));
					as.je(okay);
					emit_pc(dec.pc);
					as.jmp(term);
					as.bind(okay);
				}
				else if (rs2x > 0) {
					if (rs1x > 0) {
						as.mov(x86::byte_ptr(x86::gpq(rs1x), dec.imm), x86::gpb_lo(rs2x));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.mov(x86::byte_ptr(x86::rax, dec.imm), x86::gpb_lo(rs2x));
					}
				}
				else {
					as.mov(x86::rcx, rbp_reg_q(dec.rs2));
					if (rs1x > 0) {
						as.mov(x86::byte_ptr(x86::gpq(rs1x), dec.imm), x86::cl);
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
						as.mov(x86::byte_ptr(x86::rax, dec.imm), x86::cl);
					}
				}
			}
			return true;
		}

		bool emit_lui(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + inst_length(dec.inst);
			int rdx = x86_reg(dec.rd);
			if (dec.rd == rv_ireg_zero) {
				// nop
			} else {
				if (rdx > 0) {
					as.mov(x86::gpq(rdx), (int)dec.imm);
				} else {
					as.mov(rbp_reg_q(dec.rd), (int)dec.imm);
				}
			}
			return true;
		}

		bool emit_jal(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = dec.pc + dec.imm;
			int rdx = x86_reg(dec.rd);
			if (dec.rd == rv_ireg_zero) {
				// nop
			} else {
				addr_t link_addr = dec.pc + inst_length(dec.inst);

				if (dec.rd == rv_ireg_ra) {
					callstack.push_back(link_addr);
				}

				if (dec.rd == rv_ireg_zero) {
					// ret
				}
				else if (rdx > 0) {
					as.mov(x86::gpq(rdx), Imm(link_addr));
				}
				else {
					as.mov(x86::rax, Imm(link_addr));
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}
			return true;
		}

		bool emit_jalr(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\t%s", dec.pc, disasm_inst_simple(dec).c_str());
			term_pc = 0;
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1);
			if (dec.rd == rv_ireg_zero && dec.rs1 == rv_ireg_ra && callstack.size() > 0) {
				addr_t link_addr = callstack.back();
				callstack.pop_back();

				auto etl = create_exit_tramp(dec.pc);
				if (rs1x > 0) {
					if (link_addr < std::numeric_limits<u32>::max()) {
						as.cmp(x86::gpq(rs1x), Imm(link_addr));
					} else {
						as.mov(x86::rax, Imm(link_addr));
						as.cmp(x86::gpq(rs1x), x86::rax);
					}
				} else {
					as.mov(x86::rax, Imm(link_addr));
					as.cmp(rbp_reg_q(dec.rs1), x86::rax);
				}
				as.jne(etl->second);

				return true;
			} else {
				term_pc = 0;
				addr_t link_addr = dec.pc + inst_length(dec.inst);

				if (dec.rd == rv_ireg_ra) {
					callstack.push_back(link_addr);
				}

				if (dec.rs1 == rv_ireg_zero) {
					if (dec.imm == 0) {
						as.xor_(x86::eax, x86::eax);
					} else {
						as.mov(x86::eax, Imm(dec.imm));
					}
				} else if (rs1x > 0) {
					if (dec.imm == 0) {
						as.mov(x86::qword_ptr(x86::rbp, proc_offset(pc)), x86::gpq(rs1x));
					} else {
						as.lea(x86::rax, x86::qword_ptr(x86::gpq(rs1x), dec.imm));
						as.mov(x86::qword_ptr(x86::rbp, proc_offset(pc)), x86::rax);
					}
				} else {
					as.mov(x86::rax, rbp_reg_q(dec.rs1));
					as.add(x86::rax, dec.imm);
					as.mov(x86::qword_ptr(x86::rbp, proc_offset(pc)), x86::rax);
				}

				if (dec.rd == rv_ireg_zero) {
					// ret
				}
				else if (rdx > 0) {
					as.mov(x86::gpq(rdx), Imm(link_addr));
				} else {
					as.mov(x86::rax, Imm(link_addr));
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}

				commit_instret();

				as.jmp(Imm(func_address(lookup_trace_fast)));

				return false;
			}
		}

		bool emit_la(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\tla          %s, pc + %d", dec.pc, rv_ireg_name_sym[dec.rd], dec.imm);
			term_pc = dec.pc + dec.sz;
			int rdx = x86_reg(dec.rd);
			if (dec.rd == rv_ireg_zero) {
				// nop
			} else {
				int64_t addr = dec.pc + dec.imm;
				if (dec.rd == rv_ireg_zero) {
					// nop
				} else if (rdx > 0) {
					as.mov(x86::gpq(rdx), Imm(addr));
				} else {
					as.mov(x86::rax, Imm(addr));
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}
			return true;
		}

		bool emit_call(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\tcall        %s, 0x%x", dec.pc, rv_ireg_name_sym[dec.rd], dec.imm);
			term_pc = dec.pc + dec.imm;
			int rdx = x86_reg(rv_ireg_ra), rs1x = x86_reg(dec.rd);
			addr_t link_addr = dec.pc + dec.sz;
			callstack.push_back(link_addr);

			if (dec.rd == rv_ireg_ra) {
				// nop
			} else if (rs1x > 0) {
				as.mov(x86::gpq(rs1x), Imm(term_pc));
			} else {
				as.mov(x86::rax, Imm(term_pc));
				as.mov(rbp_reg_q(dec.rs1), x86::rax);
			}

			if (rdx > 0) {
				as.mov(x86::gpq(rdx), Imm(link_addr));
			} else {
				as.mov(x86::rax, Imm(link_addr));
				as.mov(rbp_reg_q(dec.rd), x86::rax);
			}

			return true;
		}

		bool emit_zextw(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\tzext.w      %s, %s", dec.pc, rv_ireg_name_sym[dec.rd], rv_ireg_name_sym[dec.rs1]);
			term_pc = dec.pc + dec.sz;
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1);

			if (dec.rd == dec.rs1) {
				if (rdx > 0) {
					as.mov(x86::gpd(rdx), x86::gpd(rdx));
				} else {
					as.mov(x86::eax, rbp_reg_d(dec.rd));
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}
			else {
				if (rdx > 0 && rs1x > 0) {
					as.mov(x86::gpd(rdx), x86::gpd(rs1x));
				} else if (rdx > 0) {
					as.mov(x86::gpd(rdx), rbp_reg_d(dec.rs1));
				} else if (rs1x > 0) {
					as.mov(x86::eax, x86::gpd(rs1x));
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				} else {
					as.mov(x86::eax, rbp_reg_d(dec.rs1));
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}

			return true;
		}

		bool emit_addiwz(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\taddiw.z     %s, %d", dec.pc, rv_ireg_name_sym[dec.rd], dec.imm);
			term_pc = dec.pc + dec.sz;
			int rdx = x86_reg(dec.rd);

			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else {
				if (rdx > 0) {
					as.add(x86::gpd(rdx), Imm(dec.imm));
					as.movzx(x86::gpq(rdx), x86::gpd(rdx));
				} else {
					as.mov(x86::eax, rbp_reg_d(dec.rd));
					as.add(x86::eax, Imm(dec.imm));
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}

			return true;
		}

		bool emit_rorwi_rr(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\trorwi_rr    %s, %s, %s, %d", dec.pc, rv_ireg_name_sym[dec.rd],
				rv_ireg_name_sym[dec.rs1], rv_ireg_name_sym[dec.rs2], dec.imm);
			term_pc = dec.pc + dec.sz;
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rd2x = x86_reg(dec.rs2);

			if (dec.rd == dec.rs1 || dec.rs2 == dec.rs1) {
				if (rs1x > 0) {
					as.mov(x86::ecx, x86::gpd(rs1x));
					as.mov(x86::eax, x86::gpd(rs1x));
				} else {
					as.mov(x86::ecx, rbp_reg_d(dec.rs1));
					as.mov(x86::eax, x86::ecx);
				}
				as.shr(x86::ecx, Imm(dec.imm));
				as.ror(x86::eax, Imm(dec.imm));
				if (rd2x > 0) {
					as.movsxd(x86::gpq(rd2x), x86::ecx);
				} else {
					as.movsxd(x86::rcx, x86::ecx);
					as.mov(rbp_reg_q(dec.rs2), x86::rcx);
				}
				if (rdx > 0) {
					as.movsxd(x86::gpq(rdx), x86::eax);
				} else {
					as.movsxd(x86::rax, x86::eax);
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			} else {
				// rotate right residual
				if (rd2x > 0) {
					if (rs1x > 0) {
						as.mov(x86::gpd(rd2x), x86::gpd(rs1x));
					} else {
						as.mov(x86::gpd(rd2x), rbp_reg_d(dec.rs1));
					}
					as.shr(x86::gpd(rd2x), Imm(dec.imm));
					as.movsxd(x86::gpq(rd2x), x86::gpd(rd2x));
				}
				else {
					if (rs1x > 0) {
						as.mov(x86::eax, x86::gpd(rs1x));
					} else {
						as.mov(x86::eax, rbp_reg_d(dec.rs1));
					}
					as.sar(x86::eax, Imm(dec.imm));
					as.movsxd(x86::rax, x86::eax);
					as.mov(rbp_reg_q(dec.rs2), x86::rax);
				}

				// rotate
				if (rdx > 0) {
					emit_mv_rd_rs1_sx_32(dec);
					as.ror(x86::gpd(rdx), Imm(dec.imm));
					emit_sx_32_rd(dec);
				} else {
					if (rs1x > 0) {
						as.mov(x86::eax, x86::gpd(rs1x));
					} else {
						as.mov(x86::eax, rbp_reg_d(dec.rs1));
					}
					as.ror(x86::eax, Imm(dec.imm));
					as.movsxd(x86::rax, x86::eax);
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}

			return true;
		}

		bool emit_rorwi_lr(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\trorwi_lr    %s, %s, %s, %d", dec.pc, rv_ireg_name_sym[dec.rd],
				rv_ireg_name_sym[dec.rs1], rv_ireg_name_sym[dec.rs2], dec.imm);
			term_pc = dec.pc + dec.sz;
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rd2x = x86_reg(dec.rs2);

			if (dec.rd == dec.rs1 || dec.rs2 == dec.rs1) {
				if (rs1x > 0) {
					as.mov(x86::ecx, x86::gpd(rs1x));
					as.mov(x86::eax, x86::gpd(rs1x));
				} else {
					as.mov(x86::ecx, rbp_reg_d(dec.rs1));
					as.mov(x86::eax, x86::ecx);
				}
				as.shl(x86::ecx, Imm(32 - dec.imm));
				as.ror(x86::eax, Imm(dec.imm));
				if (rd2x > 0) {
					as.movsxd(x86::gpq(rd2x), x86::ecx);
				} else {
					as.movsxd(x86::rcx, x86::ecx);
					as.mov(rbp_reg_q(dec.rs2), x86::rcx);
				}
				if (rdx > 0) {
					as.movsxd(x86::gpq(rdx), x86::eax);
				} else {
					as.movsxd(x86::rax, x86::eax);
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			} else {
				// rotate left residual
				if (rd2x > 0) {
					if (rs1x > 0) {
						as.mov(x86::gpd(rd2x), x86::gpd(rs1x));
					} else {
						as.mov(x86::gpd(rd2x), rbp_reg_d(dec.rs1));
					}
					as.shl(x86::gpd(rd2x), Imm(32 - dec.imm));
					as.movsxd(x86::gpq(rd2x), x86::gpd(rd2x));
				} else {
					if (rs1x > 0) {
						as.mov(x86::eax, x86::gpd(rs1x));
					} else {
						as.mov(x86::eax, rbp_reg_d(dec.rs1));
					}
					as.shl(x86::eax, Imm(32 - dec.imm));
					as.movsxd(x86::rax, x86::eax);
					as.mov(rbp_reg_q(dec.rs2), x86::rax);
				}

				// rotate
				if (rdx > 0) {
					emit_mv_rd_rs1_sx_32(dec);
					as.ror(x86::gpd(rdx), Imm(dec.imm));
					emit_sx_32_rd(dec);
				} else {
					if (rs1x > 0) {
						as.mov(x86::eax, x86::gpd(rs1x));
					} else {
						as.mov(x86::eax, rbp_reg_d(dec.rs1));
					}
					as.ror(x86::eax, Imm(dec.imm));
					as.movsxd(x86::rax, x86::eax);
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}

			return true;
		}

		bool emit_rordi_rr(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\trordi_rr    %s, %s, %s, %d", dec.pc, rv_ireg_name_sym[dec.rd],
				rv_ireg_name_sym[dec.rs1], rv_ireg_name_sym[dec.rs2], dec.imm);
			term_pc = dec.pc + dec.sz;
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rd2x = x86_reg(dec.rs2);

			if (dec.rd == dec.rs1 || dec.rs2 == dec.rs1) {
				if (rs1x > 0) {
					as.mov(x86::rcx, x86::gpq(rs1x));
					as.mov(x86::rax, x86::gpq(rs1x));
				} else {
					as.mov(x86::rcx, rbp_reg_q(dec.rs1));
					as.mov(x86::rax, x86::rcx);
				}
				as.shr(x86::rcx, Imm(dec.imm));
				as.ror(x86::rax, Imm(dec.imm));
				if (rd2x > 0) {
					as.mov(x86::gpq(rd2x), x86::rcx);
				} else {
					as.mov(rbp_reg_q(dec.rs2), x86::rcx);
				}
				if (rdx > 0) {
					as.mov(x86::gpq(rdx), x86::rax);
				} else {
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			} else {
				// rotate right residual
				if (rd2x > 0) {
					if (rs1x > 0) {
						as.mov(x86::gpq(rd2x), x86::gpq(rs1x));
					} else {
						as.mov(x86::gpq(rd2x), rbp_reg_q(dec.rs1));
					}
					as.sar(x86::gpq(rd2x), Imm(dec.imm));
				}
				else {
					if (rs1x > 0) {
						as.mov(x86::rax, x86::gpq(rs1x));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
					}
					as.shr(x86::rax, Imm(dec.imm));
					as.mov(rbp_reg_q(dec.rs2), x86::rax);
				}

				// rotate
				if (rdx > 0) {
					emit_mv_rd_rs1(dec);
					as.ror(x86::gpq(rdx), Imm(dec.imm));
				} else {
					if (rs1x > 0) {
						as.mov(x86::rax, x86::gpq(rs1x));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
					}
					as.ror(x86::rax, Imm(dec.imm));
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}

			return true;
		}

		bool emit_rordi_lr(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\trordi_lr    %s, %s, %s, %d", dec.pc, rv_ireg_name_sym[dec.rd],
				rv_ireg_name_sym[dec.rs1], rv_ireg_name_sym[dec.rs2], dec.imm);
			term_pc = dec.pc + dec.sz;
			int rdx = x86_reg(dec.rd), rs1x = x86_reg(dec.rs1), rd2x = x86_reg(dec.rs2);

			if (dec.rd == dec.rs1 || dec.rs2 == dec.rs1) {
				if (rs1x > 0) {
					as.mov(x86::rcx, x86::gpq(rs1x));
					as.mov(x86::rax, x86::gpq(rs1x));
				} else {
					as.mov(x86::rcx, rbp_reg_q(dec.rs1));
					as.mov(x86::rax, x86::rcx);
				}
				as.shl(x86::rcx, Imm(64 - dec.imm));
				as.ror(x86::rax, Imm(dec.imm));
				if (rd2x > 0) {
					as.mov(x86::gpq(rd2x), x86::rcx);
				} else {
					as.mov(rbp_reg_q(dec.rs2), x86::rcx);
				}
				if (rdx > 0) {
					as.mov(x86::gpq(rdx), x86::rax);
				} else {
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			} else {
				// rotate left residual
				if (rd2x > 0) {
					if (rs1x > 0) {
						as.mov(x86::gpq(rd2x), x86::gpq(rs1x));
					} else {
						as.mov(x86::gpq(rd2x), rbp_reg_q(dec.rs1));
					}
					as.shl(x86::gpq(rd2x), Imm(64 - dec.imm));
				}
				else {
					if (rs1x > 0) {
						as.mov(x86::rax, x86::gpq(rs1x));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
					}
					as.shl(x86::rax, Imm(64 - dec.imm));
					as.mov(rbp_reg_q(dec.rs2), x86::rax);
				}

				// rotate
				if (rdx > 0) {
					emit_mv_rd_rs1(dec);
					as.ror(x86::gpq(rdx), Imm(dec.imm));
				} else {
					if (rs1x > 0) {
						as.mov(x86::rax, x86::gpq(rs1x));
					} else {
						as.mov(x86::rax, rbp_reg_q(dec.rs1));
					}
					as.ror(x86::rax, Imm(dec.imm));
					as.mov(rbp_reg_q(dec.rd), x86::rax);
				}
			}

			return true;
		}

		bool emit_auipc_lw(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\tauipc.lw    %s, %d", dec.pc, rv_ireg_name_sym[dec.rd], dec.imm);
			term_pc = dec.pc + dec.sz;
			int rdx = x86_reg(dec.rd);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else {
				u64 addr = dec.pc + dec.imm;
				if (use_mmu) {
					as.mov(x86::rax, Imm(addr));
					as.call(Imm(func_address(ops.lw)));
					auto okay = as.newLabel();
					as.cmp(x86::qword_ptr(x86::rbp, proc_offset(cause)), Imm(0));
					as.je(okay);
					emit_pc(dec.pc);
					as.jmp(term);
					as.bind(okay);
					if (rdx > 0) {
						as.movsxd(x86::gpq(rdx), x86::eax);
					} else {
						as.movsxd(x86::rax, x86::eax);
						as.mov(rbp_reg_q(dec.rd), x86::rax);
					}
				}
				else {
					if (addr <= std::numeric_limits<u32>::max()) {
						if (rdx > 0) {
							as.mov(x86::gpd(rdx), x86::dword_ptr(addr));
							as.movsxd(x86::gpq(rdx), x86::gpd(rdx));
						}
						else {
							as.mov(x86::eax, x86::dword_ptr(addr));
							as.movsxd(x86::rax, x86::eax);
							as.mov(rbp_reg_q(dec.rd), x86::rax);
						}
					} else {
						as.mov(x86::rax, Imm(addr));
						if (rdx > 0) {
							as.mov(x86::gpd(rdx), x86::dword_ptr(x86::rax));
							as.movsxd(x86::gpq(rdx), x86::gpd(rdx));
						}
						else {
							as.mov(x86::eax, x86::dword_ptr(x86::rax));
							as.movsxd(x86::rax, x86::eax);
							as.mov(rbp_reg_q(dec.rd), x86::rax);
						}
					}
				}
			}
			return true;
		}

		bool emit_auipc_ld(decode_type &dec)
		{
			log_trace("\t# 0x%016llx\tauipc.ld    %s, %d", dec.pc, rv_ireg_name_sym[dec.rd], dec.imm);
			term_pc = dec.pc + dec.sz;
			int rdx = x86_reg(dec.rd);
			if (dec.rd == rv_ireg_zero) {
				// nop
			}
			else {
				u64 addr = dec.pc + dec.imm;
				if (use_mmu) {
					as.mov(x86::rax, Imm(addr));
					as.call(Imm(func_address(ops.ld)));
					auto okay = as.newLabel();
					as.cmp(x86::qword_ptr(x86::rbp, proc_offset(cause)), Imm(0));
					as.je(okay);
					emit_pc(dec.pc);
					as.jmp(term);
					as.bind(okay);
					if (rdx > 0) {
						as.mov(x86::gpq(rdx), x86::rax);
					} else {
						as.mov(rbp_reg_q(dec.rd), x86::rax);
					}
				}
				else {
					if (addr <= std::numeric_limits<u32>::max()) {
						if (rdx > 0) {
							as.mov(x86::gpq(rdx), x86::qword_ptr(addr));
						}
						else {
							as.mov(x86::rax, x86::qword_ptr(addr));
							as.mov(rbp_reg_q(dec.rd), x86::rax);
						}
					} else {
						as.mov(x86::rax, Imm(addr));
						if (rdx > 0) {
							as.mov(x86::gpq(rdx), x86::qword_ptr(x86::rax));
						}
						else {
							as.mov(x86::rax, x86::qword_ptr(x86::rax));
							as.mov(rbp_reg_q(dec.rd), x86::rax);
						}
					}
				}
			}
			return true;
		}

		bool emit(decode_type &dec)
		{
			auto li = labels.find(dec.pc);
			if (li != labels.end()) {
				return false; /* trace complete */
			}
			Label l = as.newLabel();
			labels[dec.pc] = l;
			as.bind(l);
			switch(dec.op) {
				case rv_op_auipc:     instret++;   return emit_auipc(dec);
				case rv_op_add:       instret++;    return emit_add(dec);
				case rv_op_sub:       instret++;    return emit_sub(dec);
				case rv_op_mul:       instret++;    return emit_mul(dec);
				case rv_op_mulh:      instret++;    return emit_mulh(dec);
				case rv_op_mulhu:     instret++;    return emit_mulhu(dec);
				case rv_op_mulhsu:    instret++;    return emit_mulhsu(dec);
				case rv_op_div:       instret++;    return emit_div(dec);
				case rv_op_rem:       instret++;    return emit_rem(dec);
				case rv_op_divu:      instret++;    return emit_divu(dec);
				case rv_op_remu:      instret++;    return emit_remu(dec);
				case rv_op_slt:       instret++;    return emit_slt(dec);
				case rv_op_sltu:      instret++;    return emit_sltu(dec);
				case rv_op_and:       instret++;    return emit_and(dec);
				case rv_op_or:        instret++;    return emit_or(dec);
				case rv_op_xor:       instret++;    return emit_xor(dec);
				case rv_op_sll:       instret++;    return emit_sll(dec);
				case rv_op_srl:       instret++;    return emit_srl(dec);
				case rv_op_sra:       instret++;    return emit_sra(dec);
				case rv_op_addw:      instret++;    return emit_addw(dec);
				case rv_op_subw:      instret++;    return emit_subw(dec);
				case rv_op_mulw:      instret++;    return emit_mulw(dec);
				case rv_op_divw:      instret++;    return emit_divw(dec);
				case rv_op_remw:      instret++;    return emit_remw(dec);
				case rv_op_divuw:     instret++;    return emit_divuw(dec);
				case rv_op_remuw:     instret++;    return emit_remuw(dec);
				case rv_op_sllw:      instret++;    return emit_sllw(dec);
				case rv_op_srlw:      instret++;    return emit_srlw(dec);
				case rv_op_sraw:      instret++;    return emit_sraw(dec);
				case rv_op_addi:      instret++;    return emit_addi(dec);
				case rv_op_slti:      instret++;    return emit_slti(dec);
				case rv_op_sltiu:     instret++;    return emit_sltiu(dec);
				case rv_op_andi:      instret++;    return emit_andi(dec);
				case rv_op_ori:       instret++;    return emit_ori(dec);
				case rv_op_xori:      instret++;    return emit_xori(dec);
				case rv_op_slli:      instret++;    return emit_slli(dec);
				case rv_op_srli:      instret++;    return emit_srli(dec);
				case rv_op_srai:      instret++;    return emit_srai(dec);
				case rv_op_addiw:     instret++;    return emit_addiw(dec);
				case rv_op_slliw:     instret++;    return emit_slliw(dec);
				case rv_op_srliw:     instret++;    return emit_srliw(dec);
				case rv_op_sraiw:     instret++;    return emit_sraiw(dec);
				case rv_op_bne:       instret++;    return emit_bne(dec);
				case rv_op_beq:       instret++;    return emit_beq(dec);
				case rv_op_blt:       instret++;    return emit_blt(dec);
				case rv_op_bge:       instret++;    return emit_bge(dec);
				case rv_op_bltu:      instret++;    return emit_bltu(dec);
				case rv_op_bgeu:      instret++;    return emit_bgeu(dec);
				case rv_op_ld:        instret++;    return emit_ld(dec);
				case rv_op_lw:        instret++;    return emit_lw(dec);
				case rv_op_lwu:       instret++;    return emit_lwu(dec);
				case rv_op_lh:        instret++;    return emit_lh(dec);
				case rv_op_lhu:       instret++;    return emit_lhu(dec);
				case rv_op_lb:        instret++;    return emit_lb(dec);
				case rv_op_lbu:       instret++;    return emit_lbu(dec);
				case rv_op_sd:        instret++;    return emit_sd(dec);
				case rv_op_sw:        instret++;    return emit_sw(dec);
				case rv_op_sh:        instret++;    return emit_sh(dec);
				case rv_op_sb:        instret++;    return emit_sb(dec);
				case rv_op_lui:       instret++;    return emit_lui(dec);
				case rv_op_jal:       instret++;    return emit_jal(dec);
				case rv_op_jalr:      instret++;    return emit_jalr(dec);
				case jit_op_la:       instret += 2; return emit_la(dec);
				case jit_op_call:     instret += 2; return emit_call(dec);
				case jit_op_zextw:    instret += 2; return emit_zextw(dec);
				case jit_op_addiwz:   instret += 3; return emit_addiwz(dec);
				case jit_op_rorwi_rr: instret += 3; return emit_rorwi_rr(dec);
				case jit_op_rorwi_lr: instret += 3; return emit_rorwi_lr(dec);
				case jit_op_rordi_rr: instret += 3; return emit_rordi_rr(dec);
				case jit_op_rordi_lr: instret += 3; return emit_rordi_lr(dec);
				case jit_op_auipc_lw: instret += 2; return emit_auipc_lw(dec);
				case jit_op_auipc_ld: instret += 2; return emit_auipc_ld(dec);
			}
			return false;
		}
	};

}

#endif
