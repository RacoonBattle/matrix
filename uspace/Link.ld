/*
 * Userspace Application Linker Script
 * 2012, Ted Chang
 */
OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)
STARTUP(../bin/sdk/crt1.o)
INPUT(../bin/sdk/libinit.o)
INPUT(../bin/sdk/syscalls.o)
INPUT(../bin/sdk/div64.o)
INPUT(../bin/sdk/string.o)
INPUT(../bin/sdk/vsprintf.o)
INPUT(../bin/sdk/stdio.o)
INPUT(../bin/sdk/sprintf.o)
INPUT(../bin/sdk/printf.o)
INPUT(../bin/sdk/format.o)
INPUT(../bin/sdk/time.o)
phys = 0x20000000;
SECTIONS
{
	/*
	 * Actual code
	 */
	.text phys : AT(phys) {
		code = .;
		*(.text)
		*(.rodata)
		. = ALIGN(4096);
	}
	/*
	 * data
	 */
	.data : AT(phys + (data - code))
	{
		data = .;
		*(.data)
		. = ALIGN(4096);
	}
	/*
	 * Statically defined, uninitialized values
	 */
	.bss : AT(phys + (bss - code))
	{
		bss = .;
		*(.bss)
		. = ALIGN(4096);
	}
	/*
	 * Get rid of unnecessary GCC bits.
	 */
	/DISCARD/ :
	{
		*(.comment)
		*(.eh_frame)
		*(.note.gnu.build-id)
	}
}