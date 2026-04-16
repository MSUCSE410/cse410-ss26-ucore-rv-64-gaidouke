#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "proc.h"

// Project 4
typedef struct {
    uint64 dev; // device ID where the inode resides
    uint64 ino; // inode number (unique identifier for file)
    uint32 mode; // file type (file/dir)
    uint32 nlink; // number of hard links pointing to this inode
    uint64 pad[7]; // reserved space
} Stat;

uint64 console_write(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	tracef("write size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return len;
}

uint64 console_read(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	tracef("read size = %d", len);
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

uint64 sys_write(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_write(va, len);
	case FD_INODE:
		return inodewrite(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_read(va, len);
	case FD_INODE:
		return inoderead(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
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
	debugf("fork!");
	return fork();
}

static inline uint64 fetchaddr(pagetable_t pagetable, uint64 va)
{
	uint64 *addr = (uint64 *)useraddr(pagetable, va);
	return *addr;
}

uint64 sys_exec(uint64 path, uint64 uargv)
{
	struct proc *p = curr_proc();
	char name[MAX_STR_LEN];
	copyinstr(p->pagetable, name, path, MAX_STR_LEN);
	uint64 arg;
	static char strpool[MAX_ARG_NUM][MAX_STR_LEN];
	char *argv[MAX_ARG_NUM];
	int i;
	for (i = 0; uargv && (arg = fetchaddr(p->pagetable, uargv));
	     uargv += sizeof(char *), i++) {
		copyinstr(p->pagetable, (char *)strpool[i], arg, MAX_STR_LEN);
		argv[i] = (char *)strpool[i];
	}
	argv[i] = NULL;
	return exec(name, (char **)argv);
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
    if (copyinstr(p->pagetable, name, va, sizeof(name)) < 0){
        return -1;
	}

	struct inode *ip = namei(name);
	if (ip == 0)
		return -1;

	struct proc *np = allocproc();
	if (np == NULL) {
		iput(ip);
		return -1;
	}

	if (bin_loader(ip, np) < 0) {
		iput(ip);
		np->state = UNUSED;
		return -1;
	}

	iput(ip);

    np->parent = p;
    // stride fields already set by allocproc
    add_task(np); // make the child runnable and add it to the scheduler queue
    return np->pid;
}

uint64 sys_set_priority(long long prio)
{
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

uint64 sys_openat(uint64 va, uint64 omode, uint64 _flags)
{
	struct proc *p = curr_proc();
	char path[200];
	copyinstr(p->pagetable, path, va, 200);
	return fileopen(path, omode);
}

uint64 sys_close(int fd)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d", fd);
		return -1;
	}
	fileclose(f);
	p->files[fd] = 0;
	return 0;
}

// Project 4: this function retrieves file metadata from an open file descriptor
int sys_fstat(int fd, uint64 stat){
	//TODO: your job is to complete the syscall
	
	struct proc *p = curr_proc();

	// check if file descriptor is valid
    if (fd < 0 || fd >= FD_BUFFER_SIZE)
        return -1;

	// get file structure from process file table
    struct file *f = p->files[fd];

    if (f == NULL)
        return -1;

	// get inode from file
    struct inode *ip = f->ip;

    if (ip == NULL)
        return -1;

    ivalid(ip); // reads the inode from disk if necessary

    Stat kst; // kernel-space stat struct
    kst.dev   = ip->dev; // device id
    kst.ino   = ip->inum; // inode number
    kst.nlink = ip->nlink; // number of hard links
    //kst.size  = ip->size;
	kst.mode = (ip->type == T_FILE) ? STAT_FILE : STAT_DIR; // set mode depending on file type

	// copy stat struct from kernel to user space
    if (copyout(p->pagetable, stat, (char *)&kst, sizeof(kst)) < 0)
        return -1;

    return 0;
}

// Project 4: function that creates a hard link (another directory entry) to an existing file
int sys_linkat(int olddirfd, uint64 oldpath, int newdirfd, uint64 newpath, uint64 flags){
	//TODO: your job is to complete the syscall
	
	struct proc *p = curr_proc();

	// buffers to store user paths
    char oldName[MAXPATH], newName[MAXPATH];

	// copy old and new path from user space
    copyinstr(p->pagetable, oldName, oldpath, MAXPATH);
    copyinstr(p->pagetable, newName, newpath, MAXPATH);

	// don't allow linking a file to itself (with the same name)
    if (strncmp(oldName, newName, MAXPATH) == 0)
        return -1;

	// find inode of the original file
    struct inode *ip = namei(oldName);

    if (ip == NULL)
        return -1;

    ivalid(ip); // reads the inode from disk if necessary

	// don't allow linking directories (can break the filesystem structure)
    if (ip->type == T_DIR) {
        iput(ip);
        return -1;
    }

    ip->nlink++;
    iupdate(ip); // write updated inode to disk

    struct inode *dp = root_dir(); // get root directory (ignore dirfd)

	// create new directory entry pointing to same inode
    if (dirlink(dp, newName, ip->inum) < 0) {
		// rollback if linking fails
        ip->nlink--;
        iupdate(ip);
        iput(ip); // release file inode
        iput(dp); // release directory inode
        return -1;
    }

    iput(dp); // release directory inode
    iput(ip); // release file inode
    return 0;
}

// Project 4: this function removes a directory entry
int sys_unlinkat(int dirfd, uint64 name, uint64 flags){
	//TODO: your job is to complete the syscall
	
	struct proc *p = curr_proc();
	
    char path[MAXPATH]; // buffer for path
    copyinstr(p->pagetable, path, name, MAXPATH); // copy path from user space

    struct inode *dp = root_dir(); // get root directory (ignore dirfd)
    ivalid(dp); // reads the inode from disk if necessary

    uint offset; 
    struct inode *ip = dirlookup(dp, path, &offset); // find inode corresponding to the file name

	// file not found
    if (ip == NULL) {
        iput(dp); // release directory inode
        return -1;
    }

    ivalid(ip); // reads the inode from disk if necessary

	// don't allow unlinking directories
    if (ip->type == T_DIR) {
        iput(ip); // release inode
        iput(dp); // release directory inode
        return -1;
    }

	// clear directory entry (remove link)
    struct dirent de;
    memset(&de, 0, sizeof(de));

	// overwrite directory entry with empty entry
    if (writei(dp, 0, (uint64)&de, offset, sizeof(de)) != sizeof(de)) {
        iput(ip); // release inode
        iput(dp); // release directory inode
        return -1;
    }
    iput(dp); // release directory inode

    ip->nlink--;
    iupdate(ip); // update inode on disk
    iput(ip); // release inode

    return 0;
}

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7;
	uint64 ret;
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
	case SYS_openat:
		ret = sys_openat(args[0], args[1], args[2]);
		break;
	case SYS_close:
		ret = sys_close(args[0]);
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
		ret = sys_exec(args[0], args[1]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_fstat:
	    ret = sys_fstat(args[0],args[1]);
		break;
	case SYS_linkat:
	    ret = sys_linkat(args[0],args[1],args[2],args[3],args[4]);
		break;
	case SYS_unlinkat:
	    ret = sys_unlinkat(args[0],args[1],args[2]);
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
