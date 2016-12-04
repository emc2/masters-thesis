	.text
.globl _prog_main
.globl _do_cc_thread_destroy
.globl _do_cc_thread_create
.globl _do_cc_thread_update_immediate
.globl _do_cc_sched_cycle
_do_cc_thread_destroy:
	movl	12(%esp),%ecx
	addl	$-16,%ecx
	movl	%esp,12(%ecx)
	movl	4(%esp),%edx
	movl	%edx,(%ecx)
	movl	8(%esp),%edx
	movl	%edx,4(%ecx)
	movl	%ecx,%esp
	call	_cc_thread_destroy
	movl	12(%esp),%esp
	ret
_do_cc_thread_create:
	movl	24(%esp),%ecx
	addl	$-32,%ecx
	movl	%esp,28(%ecx)
	movl	4(%esp),%edx
	movl	%edx,(%ecx)
	movl	8(%esp),%edx
	movl	%edx,4(%ecx)
	movl	12(%esp),%edx
	movl	%edx,8(%ecx)
	movl	16(%esp),%edx
	movl	%edx,12(%ecx)
	movl	20(%esp),%edx
	movl	%edx,16(%ecx)
	movl	%ecx,%esp
	call	_cc_thread_create
	movl	28(%esp),%esp
	ret
_do_cc_thread_update_immediate:
	movl	20(%esp),%ecx
	addl	$-32,%ecx
	movl	%esp,28(%ecx)
	movl	4(%esp),%edx
	movl	%edx,(%ecx)
	movl	8(%esp),%edx
	movl	%edx,4(%ecx)
	movl	12(%esp),%edx
	movl	%edx,8(%ecx)
	movl	16(%esp),%edx
	movl	%edx,12(%ecx)
	movl	%ecx,%esp
	call	_cc_thread_update_immediate
	movl	28(%esp),%esp
	ret
_do_cc_sched_cycle:
	movl	4(%esp),%edx
	movl	8(%esp),%ecx
	movl	%ecx,%esp
	addl	$-16,%esp
	movl	%edx,(%esp)
	movl	%ecx,12(%ecx)
	call	_cc_sched_cycle
_prog_main:
	addl	$-12,%esp
# Allocate a stack
	movl	$0x8000,(%esp)
	call	L_malloc$stub
# Switch to it
	addl	$0x7fe0,%eax
	movl	16(%esp),%edi
	movl	20(%esp),%esi
	movl	24(%esp),%ebx
	movl	28(%esp),%ecx
	movl	32(%esp),%edx
	movl	%edi,4(%eax)
	movl	%esi,8(%eax)
	movl	%ebx,12(%eax)
	movl	%ecx,16(%eax)
	movl	%edx,20(%eax)
	movl	%eax,%esp
	addl	$-0x7fe0,%eax
	movl	%eax,(%esp)
	call	_entry
	.section __IMPORT,__jump_table,symbol_stubs,self_modifying_code+pure_instructions,5
L_malloc$stub:
	.indirect_symbol _malloc
	hlt
	hlt
	hlt
	hlt
	hlt
