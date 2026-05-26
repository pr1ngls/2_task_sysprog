#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pgtable.h>
#include <linux/pid.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

static pid_t target_pid;
static DEFINE_MUTEX(pt_lock);

static void emit_pte(struct seq_file *m, unsigned long addr, pte_t pte) {
  seq_printf(m, "          PTE  va=0x%016lx pfn=0x%010lx [%c%c%c%c%c]\n", addr,
             (unsigned long)pte_pfn(pte), pte_present(pte) ? 'P' : '-',
             pte_write(pte) ? 'W' : 'R', pte_exec(pte) ? 'X' : '-',
             pte_dirty(pte) ? 'D' : '-', pte_young(pte) ? 'A' : '-');
}

static void walk_pte_range(struct seq_file *m, struct mm_struct *mm, pmd_t *pmd,
                           unsigned long addr, unsigned long end) {
  pte_t *pte;
  unsigned long a;

  pte = pte_offset_kernel(pmd, addr);
  for (a = addr; a < end; a += PAGE_SIZE, pte++) {
    pte_t ptent = ptep_get(pte);

    if (pte_none(ptent) || !pte_present(ptent))
      continue;
    emit_pte(m, a, ptent);
  }
}

static void walk_pmd_range(struct seq_file *m, struct mm_struct *mm, pud_t *pud,
                           unsigned long addr, unsigned long end) {
  pmd_t *pmd;
  unsigned long next, a;

  pmd = pmd_offset(pud, addr);
  a = addr;
  do {
    pmd_t pmdval = pmdp_get_lockless(pmd);

    next = pmd_addr_end(a, end);
    if (pmd_none(pmdval))
      continue;

    if (pmd_leaf(pmdval)) {
      seq_printf(m, "        PMD  va=0x%016lx HUGE  pfn=0x%lx\n", a,
                 (unsigned long)pmd_pfn(pmdval));
      continue;
    }
    if (!pmd_present(pmdval))
      continue;

    seq_printf(m, "        PMD  va=0x%016lx\n", a);
    walk_pte_range(m, mm, pmd, a, next);
  } while (pmd++, a = next, a < end);
}

static void walk_pud_range(struct seq_file *m, struct mm_struct *mm, p4d_t *p4d,
                           unsigned long addr, unsigned long end) {
  pud_t *pud;
  unsigned long next, a;

  pud = pud_offset(p4d, addr);
  a = addr;
  do {
    pud_t pudval = READ_ONCE(*pud);

    next = pud_addr_end(a, end);
    if (pud_none(pudval) || !pud_present(pudval))
      continue;

    if (pud_leaf(pudval)) {
      seq_printf(m, "      PUD  va=0x%016lx GIGAPAGE pfn=0x%lx\n", a,
                 (unsigned long)pud_pfn(pudval));
      continue;
    }
#if CONFIG_PGTABLE_LEVELS > 3
    seq_printf(m, "      PUD  va=0x%016lx\n", a);
#endif
    walk_pmd_range(m, mm, pud, a, next);
  } while (pud++, a = next, a < end);
}

static void walk_p4d_range(struct seq_file *m, struct mm_struct *mm, pgd_t *pgd,
                           unsigned long addr, unsigned long end) {
  p4d_t *p4d;
  unsigned long next, a;

  p4d = p4d_offset(pgd, addr);
  a = addr;
  do {
    p4d_t p4dval = READ_ONCE(*p4d);

    next = p4d_addr_end(a, end);
    if (p4d_none(p4dval) || !p4d_present(p4dval))
      continue;

#if CONFIG_PGTABLE_LEVELS > 4
    seq_printf(m, "    P4D  va=0x%016lx\n", a);
#endif
    walk_pud_range(m, mm, p4d, a, next);
  } while (p4d++, a = next, a < end);
}

static void walk_pgd_range(struct seq_file *m, struct mm_struct *mm,
                           unsigned long start, unsigned long end) {
  pgd_t *pgd;
  unsigned long next, a;

  pgd = pgd_offset(mm, start);
  a = start;
  do {
    pgd_t pgdval = READ_ONCE(*pgd);

    next = pgd_addr_end(a, end);
    if (pgd_none(pgdval) || !pgd_present(pgdval))
      continue;
    seq_printf(m, "  PGD  va=0x%016lx\n", a);
    walk_p4d_range(m, mm, pgd, a, next);
  } while (pgd++, a = next, a < end);
}

static int dump_vmas(struct seq_file *m, struct mm_struct *mm) {
  struct vma_iterator vmi;
  struct vm_area_struct *vma;
  int n = 0;

  seq_printf(m, "VMAs (only the first 64 are listed):\n");
  vma_iter_init(&vmi, mm, 0);
  for_each_vma(vmi, vma) {
    const char *name = "anon";
    char *file_buf = NULL;
    char *path = NULL;

    if (vma->vm_file) {
      file_buf = (char *)__get_free_page(GFP_KERNEL);
      if (file_buf) {
        path = d_path(&vma->vm_file->f_path, file_buf, PAGE_SIZE);
        if (!IS_ERR(path))
          name = path;
        else
          name = "<file>";
      } else {
        name = "<file>";
      }
    }
    seq_printf(m, "  [0x%016lx-0x%016lx) %c%c%c %s\n", vma->vm_start,
               vma->vm_end, (vma->vm_flags & VM_READ) ? 'r' : '-',
               (vma->vm_flags & VM_WRITE) ? 'w' : '-',
               (vma->vm_flags & VM_EXEC) ? 'x' : '-', name);
    if (file_buf)
      free_page((unsigned long)file_buf);
    if (++n >= 64) {
      seq_puts(m, "  ... (truncated)\n");
      break;
    }
  }
  return 0;
}

static int pagetree_show(struct seq_file *m, void *v) {
  pid_t pid;
  struct pid *pidp;
  struct task_struct *task;
  struct mm_struct *mm;

  mutex_lock(&pt_lock);
  pid = target_pid;
  mutex_unlock(&pt_lock);

  if (!pid) {
    seq_puts(m, "pagetree: no target PID set yet.\n"
                "Usage: echo PID > /proc/pagetree && cat /proc/pagetree\n");
    return 0;
  }

  pidp = find_get_pid(pid);
  if (!pidp) {
    seq_printf(m, "pagetree: PID %d not found\n", pid);
    return 0;
  }
  task = get_pid_task(pidp, PIDTYPE_PID);
  put_pid(pidp);
  if (!task) {
    seq_printf(m, "pagetree: PID %d task not found\n", pid);
    return 0;
  }

  mm = get_task_mm(task);
  if (!mm) {
    seq_printf(m, "pagetree: PID %d has no mm (kernel thread?)\n", pid);
    put_task_struct(task);
    return 0;
  }

  seq_printf(m, "=== Page-table tree for PID %d (%s) ===\n", pid, task->comm);
  seq_printf(m, "page levels=%d, PAGE_SIZE=%lu\n", CONFIG_PGTABLE_LEVELS,
             PAGE_SIZE);
  seq_printf(m,
             "flags: P=present W=writable R=ro X=exec D=dirty A=accessed\n\n");

  if (mmap_read_lock_killable(mm)) {
    seq_puts(m, "pagetree: interrupted while taking mmap lock\n");
    mmput(mm);
    put_task_struct(task);
    return 0;
  }

  dump_vmas(m, mm);
  seq_puts(m, "\nPage table:\n");
  walk_pgd_range(m, mm, 0, TASK_SIZE);

  mmap_read_unlock(mm);
  mmput(mm);
  put_task_struct(task);
  return 0;
}

static int pagetree_open(struct inode *inode, struct file *file) {
  return single_open(file, pagetree_show, NULL);
}

static ssize_t pagetree_write(struct file *file, const char __user *buf,
                              size_t count, loff_t *ppos) {
  char kbuf[32];
  pid_t pid;
  int ret;

  if (count == 0 || count >= sizeof(kbuf))
    return -EINVAL;
  if (copy_from_user(kbuf, buf, count))
    return -EFAULT;
  kbuf[count] = '\0';

  ret = kstrtoint(strim(kbuf), 10, &pid);
  if (ret)
    return ret;
  if (pid <= 0)
    return -EINVAL;

  mutex_lock(&pt_lock);
  target_pid = pid;
  mutex_unlock(&pt_lock);
  return count;
}

static const struct proc_ops pagetree_pops = {
    .proc_open = pagetree_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
    .proc_write = pagetree_write,
};

static int __init pagetree_init(void) {
  if (!proc_create("pagetree", 0644, NULL, &pagetree_pops))
    return -ENOMEM;
  pr_info("pagetree: /proc/pagetree ready\n");
  return 0;
}

static void __exit pagetree_exit(void) {
  remove_proc_entry("pagetree", NULL);
  pr_info("pagetree: unloaded\n");
}

module_init(pagetree_init);
module_exit(pagetree_exit);
