// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test nested guests that run in ESA mode (KVM_CAP_S390_VSIE_ESAMODE).
 *
 * Guest 2 runs a guest 3 from a state description without the
 * z/Architecture flag. Such a state description has the eight-byte ESA/390
 * PSW in the first doubleword of the PSW field. vsie rewinds the PSW of
 * guest 3 when it has to repeat an instruction; the test uses STORE FACILITY
 * LIST EXTENDED, which vsie repeats after it has shadowed the facility list
 * guest 2 designates.
 */
#include <stdio.h>
#include <string.h>

#include "kvm_util.h"
#include "test_util.h"
#include "kselftest.h"
#include "sie.h"
#include "facility.h"
#include "ucall_common.h"

#define G3_MEM_LEN	0x100000UL
/* mso/msl have 1MB granularity */
#define G3_MSO		0x10000000UL
#define G3_START	(2 * 0x1000UL)
/* where guest 3 stores its facility lists, in its first page */
#define G3_STFLE_1	0xf00
#define G3_STFLE_2	0xf80

#define FAC_STFLE	7

/* bit 12 of an ESA/390 PSW is one, bit 32 selects 31-bit addressing */
#define ESA_PSW_MASK	0x0008000080000000ULL
#define ESA_PSW_ADDR	0x000000007fffffffULL

/* facility list for guest 3: only the first doubleword is looked at */
#define G3_FAC_0	0x4000100000000001ULL

static const u8 g3_code[] = {
	0xa7, 0x08, 0x00, 0x00,			/* lhi   %r0,0 */
	0xb2, 0xb0, 0x0f, 0x00,			/* stfle G3_STFLE_1 */
	0xa7, 0x08, 0x00, 0x00,			/* lhi   %r0,0 */
	0xb2, 0xb0, 0x0f, 0x80,			/* stfle G3_STFLE_2 */
	0x83, 0x00, 0x00, 0x44,			/* diag  %r0,%r0,0x44 */
};

static u64 scb_real;

static void guest_code(void)
{
	while (1) {
		asm volatile("	sie	0(%[scb])\n"
			     :
			     : [scb] "a" (scb_real)
			     : "memory", "cc");
		GUEST_SYNC(0);
	}
}

static void get_machine_feat(struct kvm_s390_vm_cpu_feat *feat)
{
	struct kvm_vm *vm = vm_create_barebones();

	kvm_device_attr_get(vm->fd, KVM_S390_VM_CPU_MODEL,
			    KVM_S390_VM_CPU_MACHINE_FEAT, feat);
	kvm_vm_free(vm);
}

static bool machine_has_sief2(void)
{
	struct kvm_s390_vm_cpu_feat feat = {};

	get_machine_feat(&feat);
	return feat.feat[0] & (1ULL << (63 - KVM_S390_VM_CPU_FEAT_SIEF2));
}

static bool machine_has_facility(int nr)
{
	struct kvm_s390_vm_cpu_processor proc = {};
	struct kvm_vm *vm = vm_create_barebones();
	bool ret;

	kvm_device_attr_get(vm->fd, KVM_S390_VM_CPU_MODEL,
			    KVM_S390_VM_CPU_PROCESSOR, &proc);
	ret = test_bit_inv(nr, (unsigned long *)proc.fac_list);
	kvm_vm_free(vm);
	return ret;
}

int main(int argc, char *argv[])
{
	struct kvm_s390_vm_cpu_feat feat = {};
	struct kvm_s390_sie_block *scb;
	u64 *g3_page0, *fac_list;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	gva_t gva, fac_gva;
	u64 psw_end;
	struct ucall uc;
	void *code;
	int i;

	TEST_REQUIRE(kvm_has_cap(KVM_CAP_S390_VSIE_ESAMODE));
	TEST_REQUIRE(machine_has_sief2());
	TEST_REQUIRE(machine_has_facility(FAC_STFLE));

	ksft_print_header();
	ksft_set_plan(3);

	/* SIEF2 must be enabled before the first vcpu is created */
	get_machine_feat(&feat);
	vm = vm_create(1);
	kvm_device_attr_set(vm->fd, KVM_S390_VM_CPU_MODEL,
			    KVM_S390_VM_CPU_PROCESSOR_FEAT, &feat);
	vm_enable_cap(vm, KVM_CAP_S390_VSIE_ESAMODE, 0);
	vcpu = vm_vcpu_add(vm, 0, guest_code);

	gva = vm_alloc_page(vm);
	scb = addr_gva2hva(vm, gva);
	memset(scb, 0, getpagesize());
	scb_real = addr_gva2gpa(vm, gva);
	sync_global_to_guest(vm, scb_real);

	fac_gva = vm_alloc_page(vm);
	fac_list = addr_gva2hva(vm, fac_gva);
	memset(fac_list, 0, getpagesize());
	fac_list[0] = G3_FAC_0;

	/* guest 3: two prefix pages and one page of code */
	for (i = 0; i < 3; i++) {
		gpa_t gpa = vm_phy_page_alloc(vm, MEM_REGION_TEST_DATA);

		virt_pg_map(vm, G3_MSO + i * getpagesize(), gpa);
		memset(addr_gpa2hva(vm, gpa), 0, getpagesize());
		if (i == 0)
			g3_page0 = addr_gpa2hva(vm, gpa);
		if (i == 2)
			code = addr_gpa2hva(vm, gpa);
	}
	memcpy(code, g3_code, sizeof(g3_code));

	scb->cpuflags = CPUSTAT_RUNNING;
	scb->ihcpu = 0xffff;
	scb->mso = G3_MSO;
	scb->msl = G3_MSO + ((G3_MEM_LEN - 1) & ~0xfffffUL);
	scb->psw_mask = ESA_PSW_MASK | G3_START;
	scb->psw_addr = 0;
	scb->fac = addr_gva2gpa(vm, fac_gva);
	TEST_ASSERT(!(scb->fac & 7) && addr_gva2gpa(vm, fac_gva) < (1UL << 31),
		    "facility list not usable for a designation");

	vcpu_run(vcpu);
	TEST_ASSERT_EQ(get_ucall(vcpu, &uc), UCALL_SYNC);
	/*
	 * The capability does not tell whether the machine (or the hypervisor
	 * this host is a guest of) runs a state description without the
	 * z/Architecture flag. If not, it answers with validity 0x0001.
	 */
	if (scb->icptcode == ICPT_VALIDITY && (scb->ipb >> 16) == 0x0001)
		ksft_exit_skip("the host cannot run nested guests in ESA mode (validity 0x0001)\n");
	TEST_ASSERT(scb->icptcode == ICPT_INST && scb->ipa == 0x8300,
		    "guest 3: icptcode 0x%02x ipa 0x%04x ipb 0x%08x psw 0x%016llx 0x%016llx",
		    scb->icptcode, scb->ipa, scb->ipb, scb->psw_mask, scb->psw_addr);

	psw_end = ESA_PSW_MASK | (G3_START + sizeof(g3_code));
	ksft_test_result(scb->psw_addr == 0,
			 "second PSW doubleword of the ESA mode guest untouched (0x%llx)\n",
			 scb->psw_addr);
	ksft_test_result((scb->psw_mask & ~0x0000300000000000ULL) == psw_end,
			 "ESA/390 PSW after the last instruction (0x%llx)\n",
			 scb->psw_mask);
	ksft_print_msg("second STFLE stored 0x%lx\n",
		       g3_page0[G3_STFLE_2 / sizeof(u64)]);
	ksft_test_result(g3_page0[G3_STFLE_1 / sizeof(u64)] == G3_FAC_0,
			 "first STFLE of the ESA mode guest executed (0x%lx)\n",
			 g3_page0[G3_STFLE_1 / sizeof(u64)]);

	kvm_vm_free(vm);
	ksft_finished();
}
