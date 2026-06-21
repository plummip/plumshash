	.file	"vortex_asm.c"
	.text
	.globl	vortexhash                      // -- Begin function vortexhash
	.p2align	2
	.type	vortexhash,@function
vortexhash:                             // @vortexhash
	.cfi_startproc
// %bb.0:
	mov	x9, #58809                      // =0xe5b9
	mov	x8, #31765                      // =0x7c15
	mov	x14, #4587                      // =0x11eb
	mov	x10, #44605                     // =0xae3d
	movk	x9, #7396, lsl #16
	movk	x8, #32586, lsl #16
	movk	x14, #4913, lsl #16
	movk	x10, #49842, lsl #16
	movk	x9, #18285, lsl #32
	movk	x8, #31161, lsl #32
	movk	x14, #18875, lsl #32
	movk	x10, #51831, lsl #32
	movk	x9, #48984, lsl #48
	movk	x8, #40503, lsl #48
	cmp	x1, #256
	movk	x14, #38096, lsl #48
	movk	x10, #34283, lsl #48
	add	x15, x0, x1
	b.lo	.LBB0_10
// %bb.1:
	mul	x11, x1, x8
	eor	x16, x2, x11
	mul	x11, x16, x14
	mul	x12, x16, x9
	mul	x13, x16, x10
	mul	x14, x16, x8
.LBB0_2:                                // =>This Inner Loop Header: Depth=1
	ldp	x16, x17, [x0]
	eor	x14, x16, x14
	add	x14, x14, x13
	eor	x13, x17, x13
	ldp	x16, x17, [x0, #16]
	ror	x14, x14, #53
	add	x13, x13, x12
	ror	x13, x13, #47
	eor	x12, x16, x12
	eor	x16, x17, x11
	add	x17, x0, #64
	add	x11, x12, x11
	add	x16, x16, x14
	cmp	x17, x15
	ror	x12, x11, #41
	ror	x11, x16, #7
	add	x0, x0, #32
	b.ls	.LBB0_2
// %bb.3:
	add	x16, x0, #8
	cmp	x16, x15
	b.ls	.LBB0_5
// %bb.4:
	mov	x16, x0
	b	.LBB0_6
.LBB0_5:                                // =>This Inner Loop Header: Depth=1
	add	x16, x0, #8
	ldr	x17, [x0], #16
	cmp	x0, x15
	mov	x0, x16
	eor	x14, x17, x14
	add	x14, x14, x13
	ror	x14, x14, #53
	b.ls	.LBB0_5
.LBB0_6:
	sub	x17, x15, x16
	mov	x15, xzr
	cmp	x17, #3
	b.le	.LBB0_20
// %bb.7:
	cmp	x17, #5
	b.gt	.LBB0_26
// %bb.8:
	cmp	x17, #4
	b.eq	.LBB0_31
// %bb.9:
	cmp	x17, #5
	b.eq	.LBB0_30
	b	.LBB0_35
.LBB0_10:
	mul	x11, x1, x8
	cmp	x1, #32
	eor	x16, x2, x11
	mul	x12, x16, x8
	mul	x11, x16, x10
	mul	x13, x16, x9
	mul	x14, x16, x14
	eor	x16, x16, x9
	b.hs	.LBB0_12
// %bb.11:
	mov	x1, x0
	b	.LBB0_13
.LBB0_12:                               // =>This Inner Loop Header: Depth=1
	ldp	x17, x1, [x0]
	ldp	x2, x3, [x0, #16]
	eor	x16, x17, x16
	eor	x12, x17, x12
	add	x17, x0, #64
	eor	x4, x1, x2
	add	x12, x12, x11
	eor	x11, x1, x11
	eor	x16, x16, x4
	ror	x12, x12, #53
	add	x11, x11, x13
	eor	x16, x16, x3
	eor	x13, x2, x13
	ror	x11, x11, #47
	ror	x16, x16, #33
	add	x13, x13, x14
	eor	x14, x3, x14
	add	x14, x14, x12
	ror	x13, x13, #41
	add	x1, x0, #32
	mul	x16, x16, x8
	ror	x14, x14, #7
	cmp	x17, x15
	mov	x0, x1
	b.ls	.LBB0_12
.LBB0_13:
	add	x17, x1, #8
	cmp	x17, x15
	b.ls	.LBB0_15
// %bb.14:
	mov	x17, x1
	b	.LBB0_16
.LBB0_15:                               // =>This Inner Loop Header: Depth=1
	add	x17, x1, #8
	ldr	x0, [x1], #16
	cmp	x1, x15
	mov	x1, x17
	eor	x16, x0, x16
	eor	x12, x0, x12
	ror	x16, x16, #33
	add	x12, x12, x11
	ror	x12, x12, #53
	mul	x16, x16, x8
	b.ls	.LBB0_15
.LBB0_16:
	sub	x0, x15, x17
	mov	x15, xzr
	cmp	x0, #3
	b.le	.LBB0_23
// %bb.17:
	cmp	x0, #5
	b.gt	.LBB0_36
// %bb.18:
	cmp	x0, #4
	b.eq	.LBB0_41
// %bb.19:
	cmp	x0, #5
	b.eq	.LBB0_40
	b	.LBB0_46
.LBB0_20:
	cmp	x17, #1
	b.eq	.LBB0_34
// %bb.21:
	cmp	x17, #2
	b.eq	.LBB0_33
// %bb.22:
	cmp	x17, #3
	b.eq	.LBB0_32
	b	.LBB0_35
.LBB0_23:
	cmp	x0, #1
	b.eq	.LBB0_44
// %bb.24:
	cmp	x0, #2
	b.eq	.LBB0_43
// %bb.25:
	cmp	x0, #3
	b.eq	.LBB0_42
	b	.LBB0_46
.LBB0_26:
	cmp	x17, #6
	b.eq	.LBB0_29
// %bb.27:
	cmp	x17, #7
	b.ne	.LBB0_35
// %bb.28:
	ldrb	w15, [x16, #6]
	lsl	x15, x15, #48
.LBB0_29:
	ldrb	w17, [x16, #5]
	orr	x15, x15, x17, lsl #40
.LBB0_30:
	ldrb	w17, [x16, #4]
	eor	x15, x15, x17, lsl #32
.LBB0_31:
	ldrb	w17, [x16, #3]
	eor	x15, x15, x17, lsl #24
.LBB0_32:
	ldrb	w17, [x16, #2]
	eor	x15, x15, x17, lsl #16
.LBB0_33:
	ldrb	w17, [x16, #1]
	eor	x15, x15, x17, lsl #8
.LBB0_34:
	ldrb	w16, [x16]
	eor	x15, x15, x16
	eor	x14, x15, x14
	add	x14, x14, x13
	ror	x14, x14, #53
.LBB0_35:
	eor	x12, x13, x12
	eor	x11, x12, x11
	eor	x11, x11, x14
	b	.LBB0_47
.LBB0_36:
	cmp	x0, #6
	b.eq	.LBB0_39
// %bb.37:
	cmp	x0, #7
	b.ne	.LBB0_46
// %bb.38:
	ldrb	w15, [x17, #6]
	lsl	x15, x15, #48
.LBB0_39:
	ldrb	w0, [x17, #5]
	orr	x15, x15, x0, lsl #40
.LBB0_40:
	ldrb	w0, [x17, #4]
	eor	x15, x15, x0, lsl #32
.LBB0_41:
	ldrb	w0, [x17, #3]
	eor	x15, x15, x0, lsl #24
.LBB0_42:
	ldrb	w0, [x17, #2]
	eor	x15, x15, x0, lsl #16
.LBB0_43:
	ldrb	w0, [x17, #1]
	eor	x15, x15, x0, lsl #8
.LBB0_44:
	ldrb	w17, [x17]
	cmp	x15, x17
	b.eq	.LBB0_46
// %bb.45:
	eor	x15, x15, x17
	eor	x16, x15, x16
	eor	x12, x15, x12
	ror	x16, x16, #33
	add	x12, x12, x11
	ror	x12, x12, #53
	mul	x16, x16, x8
.LBB0_46:
	eor	x13, x13, x14
	eor	x12, x13, x12
	eor	x11, x12, x11
	eor	x11, x11, x16
.LBB0_47:
	eor	x11, x11, x11, lsr #29
	mul	x10, x11, x10
	eor	x10, x10, x10, lsr #31
	mul	x9, x10, x9
	eor	x9, x9, x9, lsr #37
	mul	x8, x9, x8
	eor	x0, x8, x8, lsr #41
	ret
.Lfunc_end0:
	.size	vortexhash, .Lfunc_end0-vortexhash
	.cfi_endproc
                                        // -- End function
	.ident	"clang version 21.1.8"
	.section	".note.GNU-stack","",@progbits
