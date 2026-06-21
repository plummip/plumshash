	.file	"ph_v4.c"
	.text
	.globl	v2                              // -- Begin function v2
	.p2align	2
	.type	v2,@function
v2:                                     // @v2
	.cfi_startproc
// %bb.0:
	mov	x8, #31765                      // =0x7c15
	mov	x9, #44605                      // =0xae3d
	mov	x12, #4587                      // =0x11eb
	movk	x8, #32586, lsl #16
	movk	x9, #49842, lsl #16
	movk	x12, #4913, lsl #16
	movk	x8, #31161, lsl #32
	movk	x9, #51831, lsl #32
	movk	x12, #18875, lsl #32
	movk	x8, #40503, lsl #48
	movk	x9, #34283, lsl #48
	movk	x12, #38096, lsl #48
	mul	x10, x1, x8
	cmp	x1, #32
	add	x16, x0, x1
	eor	x14, x2, x10
	mov	x10, #58809                     // =0xe5b9
	movk	x10, #7396, lsl #16
	mul	x13, x14, x9
	movk	x10, #18285, lsl #32
	movk	x10, #48984, lsl #48
	mul	x12, x14, x12
	mul	x11, x14, x10
	mul	x14, x14, x8
	b.hs	.LBB0_2
// %bb.1:
	mov	x17, x0
	b	.LBB0_3
.LBB0_2:                                // =>This Inner Loop Header: Depth=1
	ldr	x15, [x0, #8]
	eor	x13, x13, x15, ror #45
	ldp	x15, x17, [x0, #16]
	mul	x13, x13, x8
	eor	x11, x11, x15, ror #29
	ldr	x15, [x0]
	eor	x12, x12, x17, ror #17
	add	x17, x0, #32
	eor	x14, x14, x15, ror #53
	mul	x11, x11, x8
	add	x15, x0, #64
	cmp	x15, x16
	mov	x0, x17
	mul	x12, x12, x8
	mul	x14, x14, x8
	b.ls	.LBB0_2
.LBB0_3:
	add	x15, x17, #8
	cmp	x15, x16
	b.ls	.LBB0_5
// %bb.4:
	mov	x15, x17
	b	.LBB0_6
.LBB0_5:                                // =>This Inner Loop Header: Depth=1
	add	x15, x17, #8
	ldr	x0, [x17], #16
	cmp	x17, x16
	mov	x17, x15
	eor	x14, x14, x0, ror #53
	mul	x14, x14, x8
	b.ls	.LBB0_5
.LBB0_6:
	sub	x17, x16, x15
	mov	x16, xzr
	cmp	x17, #3
	b.le	.LBB0_10
// %bb.7:
	cmp	x17, #5
	b.gt	.LBB0_13
// %bb.8:
	cmp	x17, #4
	b.eq	.LBB0_18
// %bb.9:
	cmp	x17, #5
	b.eq	.LBB0_17
	b	.LBB0_22
.LBB0_10:
	cmp	x17, #1
	b.eq	.LBB0_21
// %bb.11:
	cmp	x17, #2
	b.eq	.LBB0_20
// %bb.12:
	cmp	x17, #3
	b.eq	.LBB0_19
	b	.LBB0_22
.LBB0_13:
	cmp	x17, #6
	b.eq	.LBB0_16
// %bb.14:
	cmp	x17, #7
	b.ne	.LBB0_22
// %bb.15:
	ldrb	w16, [x15, #6]
	lsl	x16, x16, #48
.LBB0_16:
	ldrb	w17, [x15, #5]
	orr	x16, x16, x17, lsl #40
.LBB0_17:
	ldrb	w17, [x15, #4]
	eor	x16, x16, x17, lsl #32
.LBB0_18:
	ldrb	w17, [x15, #3]
	eor	x16, x16, x17, lsl #24
.LBB0_19:
	ldrb	w17, [x15, #2]
	eor	x16, x16, x17, lsl #16
.LBB0_20:
	ldrb	w17, [x15, #1]
	eor	x16, x16, x17, lsl #8
.LBB0_21:
	ldrb	w15, [x15]
	eor	x15, x16, x15
	eor	x14, x14, x15, ror #53
	mul	x14, x14, x8
.LBB0_22:
	eor	x11, x11, x13
	eor	x11, x11, x12
	eor	x11, x11, x14
	eor	x11, x11, x11, lsr #29
	mul	x9, x11, x9
	eor	x9, x9, x9, lsr #31
	mul	x9, x9, x10
	eor	x9, x9, x9, lsr #37
	mul	x8, x9, x8
	eor	x0, x8, x8, lsr #41
	ret
.Lfunc_end0:
	.size	v2, .Lfunc_end0-v2
	.cfi_endproc
                                        // -- End function
	.globl	v4x                             // -- Begin function v4x
	.p2align	2
	.type	v4x,@function
v4x:                                    // @v4x
	.cfi_startproc
// %bb.0:
	mov	x8, #31765                      // =0x7c15
	mov	x9, #44605                      // =0xae3d
	mov	x13, #4587                      // =0x11eb
	movk	x8, #32586, lsl #16
	movk	x9, #49842, lsl #16
	movk	x13, #4913, lsl #16
	movk	x8, #31161, lsl #32
	movk	x9, #51831, lsl #32
	movk	x13, #18875, lsl #32
	movk	x8, #40503, lsl #48
	movk	x9, #34283, lsl #48
	movk	x13, #38096, lsl #48
	mul	x10, x1, x8
	cmp	x1, #128
	add	x15, x0, x1
	eor	x12, x2, x10
	mov	x10, #58809                     // =0xe5b9
	movk	x10, #7396, lsl #16
	mul	x14, x12, x9
	movk	x10, #18285, lsl #32
	movk	x10, #48984, lsl #48
	mul	x13, x12, x13
	mul	x11, x12, x10
	mul	x12, x12, x8
	b.lo	.LBB1_2
.LBB1_1:                                // =>This Inner Loop Header: Depth=1
	prfm	pldl1keep, [x0, #256]
	ldp	x17, x16, [x0, #8]
	eor	x14, x14, x17, ror #45
	ldr	x17, [x0, #24]
	eor	x11, x11, x16, ror #29
	ldr	x16, [x0]
	mul	x14, x14, x8
	eor	x13, x13, x17, ror #17
	ldr	x17, [x0, #40]
	eor	x12, x12, x16, ror #53
	mul	x11, x11, x8
	mul	x13, x13, x8
	eor	x14, x14, x17, ror #45
	ldp	x16, x17, [x0, #48]
	mul	x12, x12, x8
	mul	x14, x14, x8
	eor	x11, x11, x16, ror #29
	ldr	x16, [x0, #32]
	eor	x13, x13, x17, ror #17
	ldr	x17, [x0, #72]
	mul	x11, x11, x8
	eor	x12, x12, x16, ror #53
	mul	x13, x13, x8
	eor	x14, x14, x17, ror #45
	ldp	x16, x17, [x0, #80]
	mul	x12, x12, x8
	mul	x14, x14, x8
	eor	x11, x11, x16, ror #29
	ldr	x16, [x0, #64]
	eor	x13, x13, x17, ror #17
	ldr	x17, [x0, #104]
	mul	x11, x11, x8
	eor	x12, x12, x16, ror #53
	mul	x13, x13, x8
	eor	x14, x14, x17, ror #45
	ldp	x16, x17, [x0, #112]
	mul	x12, x12, x8
	eor	x11, x11, x16, ror #29
	ldr	x16, [x0, #96]
	mul	x14, x14, x8
	eor	x13, x13, x17, ror #17
	mul	x11, x11, x8
	eor	x12, x12, x16, ror #53
	add	x16, x0, #256
	add	x0, x0, #128
	mul	x13, x13, x8
	cmp	x16, x15
	mul	x12, x12, x8
	b.ls	.LBB1_1
.LBB1_2:
	add	x16, x0, #32
	cmp	x16, x15
	b.ls	.LBB1_4
// %bb.3:
	mov	x17, x0
	b	.LBB1_5
.LBB1_4:                                // =>This Inner Loop Header: Depth=1
	ldp	x16, x17, [x0]
	eor	x12, x12, x16, ror #53
	eor	x14, x14, x17, ror #45
	ldp	x16, x17, [x0, #16]
	mul	x12, x12, x8
	eor	x11, x11, x16, ror #29
	eor	x13, x13, x17, ror #17
	mul	x14, x14, x8
	add	x16, x0, #64
	add	x17, x0, #32
	mul	x11, x11, x8
	cmp	x16, x15
	mov	x0, x17
	mul	x13, x13, x8
	b.ls	.LBB1_4
.LBB1_5:
	add	x16, x17, #8
	cmp	x16, x15
	b.ls	.LBB1_7
// %bb.6:
	mov	x16, x17
	b	.LBB1_8
.LBB1_7:                                // =>This Inner Loop Header: Depth=1
	add	x16, x17, #8
	ldr	x0, [x17], #16
	cmp	x17, x15
	mov	x17, x16
	eor	x12, x12, x0, ror #53
	mul	x12, x12, x8
	b.ls	.LBB1_7
.LBB1_8:
	sub	x17, x15, x16
	mov	x15, xzr
	cmp	x17, #3
	b.le	.LBB1_12
// %bb.9:
	cmp	x17, #5
	b.gt	.LBB1_15
// %bb.10:
	cmp	x17, #4
	b.eq	.LBB1_20
// %bb.11:
	cmp	x17, #5
	b.eq	.LBB1_19
	b	.LBB1_24
.LBB1_12:
	cmp	x17, #1
	b.eq	.LBB1_23
// %bb.13:
	cmp	x17, #2
	b.eq	.LBB1_22
// %bb.14:
	cmp	x17, #3
	b.eq	.LBB1_21
	b	.LBB1_24
.LBB1_15:
	cmp	x17, #6
	b.eq	.LBB1_18
// %bb.16:
	cmp	x17, #7
	b.ne	.LBB1_24
// %bb.17:
	ldrb	w15, [x16, #6]
	lsl	x15, x15, #48
.LBB1_18:
	ldrb	w17, [x16, #5]
	orr	x15, x15, x17, lsl #40
.LBB1_19:
	ldrb	w17, [x16, #4]
	eor	x15, x15, x17, lsl #32
.LBB1_20:
	ldrb	w17, [x16, #3]
	eor	x15, x15, x17, lsl #24
.LBB1_21:
	ldrb	w17, [x16, #2]
	eor	x15, x15, x17, lsl #16
.LBB1_22:
	ldrb	w17, [x16, #1]
	eor	x15, x15, x17, lsl #8
.LBB1_23:
	ldrb	w16, [x16]
	eor	x15, x15, x16
	eor	x12, x12, x15, ror #53
	mul	x12, x12, x8
.LBB1_24:
	eor	x11, x11, x14
	eor	x11, x11, x13
	eor	x11, x11, x12
	eor	x11, x11, x11, lsr #29
	mul	x9, x11, x9
	eor	x9, x9, x9, lsr #31
	mul	x9, x9, x10
	eor	x9, x9, x9, lsr #37
	mul	x8, x9, x8
	eor	x0, x8, x8, lsr #41
	ret
.Lfunc_end1:
	.size	v4x, .Lfunc_end1-v4x
	.cfi_endproc
                                        // -- End function
	.ident	"clang version 21.1.8"
	.section	".note.GNU-stack","",@progbits
