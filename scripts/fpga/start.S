.section .text.start,"ax",@progbits
.globl _start
.type _start, @function
_start:
  la gp, __global_pointer$
  li sp, 0xbff00000

  la t0, __bss_start
  la t1, __bss_end
.Lzero_bss:
  bgeu t0, t1, .Lbss_done
  sd zero, 0(t0)
  addi t0, t0, 8
  j .Lzero_bss
.Lbss_done:
  call main

  li t0, 0xcf000000
  sw a0, 0(t0)
  li t1, 1
  sw t1, 4(t0)

.globl __done
__done:
  j __done
.size _start, .-_start
