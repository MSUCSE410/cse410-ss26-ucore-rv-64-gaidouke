#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "proc.h"


uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d str = %x, len = %d", fd, va, len);
	if (fd != STDOUT)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	debugf("size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return size;
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	debugf("sys_read fd = %d str = %x, len = %d", fd, va, len);
	if (fd != STDIN)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

__attribute__((noreturn)) void sys_exit(int code)
{
	exit(code);
	__builtin_unreachable();
}

uint64 sys_sched_yield()
{
	yield();
	return 0;
}

uint64 sys_gettimeofday(uint64 val, int _tz)
{
	struct proc *p = curr_proc();
	uint64 cycle = get_cycle();
	TimeVal t;
	t.sec = cycle / CPU_FREQ;
	t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal));
	return 0;
}

uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd){
	if (len == 0)
        return 0;

	// start must be page-aligned
    if (start % PGSIZE != 0)
        return -1;

    if (len > (1UL << 30)) // >1GiB
        return -1;

    // invalid port bits
    if (port & ~0x7)
        return -1;

    // meaningless permission
    if ((port & 0x7) == 0)
        return -1;

    struct proc *p = curr_proc();
    pagetable_t pagetable = p->pagetable;

    uint64 a = PGROUNDDOWN(start);
    uint64 last = PGROUNDUP(start + len);

    // check if already mapped
    for (uint64 va = a; va < last; va += PGSIZE) {
        pte_t *pte = walk(pagetable, va, 0);
        if (pte && (*pte & PTE_V))
            return -1;
    }

    // build permission flags
    int perm = PTE_U;
    if (port & 1) perm |= PTE_R;
    if (port & 2) perm |= PTE_W;
    if (port & 4) perm |= PTE_X;

    // allocate and map pages
    for (uint64 va = a; va < last; va += PGSIZE) {
        void *pa = kalloc();
        if (!pa)
            return -1;

        memset(pa, 0, PGSIZE);

        if (mappages(pagetable, va, PGSIZE, (uint64)pa, perm) != 0) {
            kfree(pa);
            return -1;
        }
    }

    return 0;
}


uint64 sys_munmap(uint64 start, uint64 len){
	if (len == 0)
        return 0;

	// must be page-aligned
    if (start % PGSIZE != 0)
        return -1;
    if (len % PGSIZE != 0)
        return -1;

    struct proc *p = curr_proc();
    pagetable_t pagetable = p->pagetable;

    uint64 a = PGROUNDDOWN(start);
    uint64 last = PGROUNDDOWN(start + len - 1);

    // Check that all pages are mapped
    for (uint64 va = a; va <= last; va += PGSIZE) {
        pte_t *pte = walk(pagetable, va, 0);
        if (!pte|| !(*pte & PTE_V))
            return -1;
    }

    // number of pages
    uint64 npages = (last - a)/PGSIZE + 1;

    // unmap and free
    uvmunmap(pagetable, a, npages, 1);

    return 0;
}

uint64 sys_getpid()
{
	return curr_proc()->pid;
}

uint64 sys_getppid()
{
	struct proc *p = curr_proc();
	return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
	debugf("fork!\n");
	return fork();
}

uint64 sys_exec(uint64 va)
{
	struct proc *p = curr_proc();
	char name[200];
	copyinstr(p->pagetable, name, va, 200);
	debugf("sys_exec %s\n", name);
	return exec(name);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

// Make new proc, run prog, & return child PID on success
uint64 sys_spawn(uint64 va)
{
    struct proc *p = curr_proc();
    char name[200];

    // Copy filename from user space
    if (copyinstr(p->pagetable, name, va, sizeof(name)) < 0)
        return -1;

    // Validate the filename by looking it up in the app table;
    // get_id_by_name returns -1 if no app with that name is loaded
    int id = get_id_by_name(name);
    if (id < 0)
        return -1;

    // Allocate a fresh proc slot directly — fork() can't be used here
	// because it returns twice, which breaks kernel control flow
    struct proc *np = allocproc();
    if (np == NULL)
        return -1;

    // Load the binary into the new process
    if (loader(id, np) < 0) {
        // loader failed — free the proc and return error
        np->state = UNUSED;
        return -1;
    }

    np->parent = p;
    // stride fields already set by allocproc
    add_task(np); // make the child runnable and add it to the scheduler queue
    return np->pid;
}


uint64 sys_set_priority(long long prio){
    // TODO: your job is to complete the sys call
    
	// Validate priority: values < 2 are invalid per the spec
    if (prio < 2)
        return -1;

    struct proc *p = curr_proc();

    // Set priority
    p->priority = (uint64)prio;

    // Update pass value. Higher priority gives a smaller pass value 
	// so it gets scheduled more often
    p->pass = BIG_STRIDE / p->priority;

    return prio;
}


extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7;
	uint64 ret; // Project 3: uint64 so large return values (e.g. priority) aren't truncated
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
	
	/*
	* LAB1: you may need to update syscall counter for task info here
	*/
	struct proc *p = curr_proc();
	if (id < MAX_SYSCALL_NUM) {
		p->syscall_times[id]++;
	}
	
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday(args[0], args[1]);
		break;
	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_getppid:
		ret = sys_getppid();
		break;
	case SYS_clone: // SYS_fork
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
		break;
	case SYS_setpriority: // added project 3
		ret = sys_set_priority((long long)args[0]);
		break;
	case SYS_mmap: // added project 2
		ret = sys_mmap(args[0], args[1], args[2], args[3], args[4]);
		break;
	case SYS_munmap: // added project 2
		ret = sys_munmap(args[0], args[1]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
