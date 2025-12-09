/*
 * Test command to manually trigger coredump save
 */

#include <zephyr/shell/shell.h>
#include <zephyr/kernel.h>
#include <memfault/panics/coredump.h>
#include <memfault/panics/arch/arm/cortex_m.h>
#include <memfault/core/platform/core.h>

static int cmd_mflt_test_save(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "Manually triggering coredump save...");

	// Get current register state (best effort - not from exception)
	sMfltRegState reg = {0};

	// Capture a fake coredump with current context
	extern void memfault_fault_handler(const sMfltRegState *regs, eMemfaultRebootReason reason);

	shell_print(sh, "Calling memfault_fault_handler...");
	memfault_fault_handler(&reg, kMfltRebootReason_Assert);

	shell_print(sh, "Coredump saved! Check with 'mflt get_core' after reboot");

	// Reboot
	k_msleep(100);
	memfault_platform_reboot();

	return 0;
}

SHELL_CMD_REGISTER(mflt_manual_save, NULL, "Manually save coredump", cmd_mflt_test_save);
