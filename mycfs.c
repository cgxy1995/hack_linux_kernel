#include <linux/latencytop.h>
#include <linux/sched.h>
#include <linux/cpumask.h>
#include <linux/slab.h>
#include <linux/profile.h>
#include <linux/interrupt.h>
#include <linux/mempolicy.h>
#include <linux/migrate.h>
#include <linux/task_work.h>
#include <linux/syscalls.h>
#include <trace/events/sched.h>
#include <linux/sysfs.h>
#include <linux/vmalloc.h>
/* Include cpufreq header to add a notifier so that cpu frequency
 *  * scaling can track the current CPU frequency
 *   */
#include <linux/cpufreq.h>
#include <linux/cpuidle.h>

#include "sched.h"
#define PERIOD 100000
struct task_struct *dummytask;
struct timeval tv={
	.tv_sec = 0,
	.tv_usec = 0,
};
long acctime = 0;
struct mycfs_rq myq;
int not_init = 1;
 #define MYRB_ROOT (struct rb_root) { NULL, }
#define for_each_sched_entity(se) \
		for (; se; se = se->parent)

static inline struct task_struct *task_of(struct sched_entity *se)
{
	return container_of(se, struct task_struct, se);
}
static inline struct mycfs_rq *mycfs_rq_of(struct sched_entity *se)
{
	return se->mycfs_rq;
}
asmlinkage long sys_testsyscall(int num){
	printk(KERN_EMERG "syscall success! num is %d\n",num);
	return 0;
}
asmlinkage long sys_sched_setlimit(int limit,int pid){
	struct rq *rq = rq = cpu_rq(0);
	//inorder_setlimit() //to microsec
	unsigned long tlimit= (unsigned long)limit * 1000;
	if(limit==0)
		tlimit = 1000 * 200;
	rq->mycfs.curr->timelimit = tlimit;
	rq->mycfs.curr->timerunned = 0;
	printk(KERN_EMERG "Limit set to %d ms, addr of se is %lx\n",limit,rq->mycfs.curr);
	return 0;
}
EXPORT_SYMBOL_GPL(sys_testsyscall);
static inline struct rq *myrq_of(struct mycfs_rq *mycfs_rq)
{
	return mycfs_rq->rq;
}
static inline int entity_before(struct sched_entity *a,
				struct sched_entity *b)
{
	//printk(KERN_EMERG "a time %lld, b time %lld\n",a->vruntime,b->vruntime);
	return (s64)(a->vruntime - b->vruntime) < 0;
}
static inline struct cfs_rq *task_mycfs_rq(struct task_struct *p)
{
	return p->se.mycfs_rq;
}
void pre_clear_runtime(struct rb_node *head){
	struct sched_entity *se;
	if(!head)
		return;
	se = rb_entry(head, struct  sched_entity, run_node);
	se->timerunned = 0;
	pre_clear_runtime(head->rb_left);
	pre_clear_runtime(head->rb_right);
}
void clear_runtime(void){
	struct rq *rq = cpu_rq(0);
	struct mycfs_rq *mycfs_rq = &rq->mycfs;
	pre_clear_runtime(mycfs_rq->root.rb_node);
}
void inorder(struct rb_node *head){
	if(!head){
		//printk(KERN_EMERG "inorde return\n");
		return;
	}
	inorder(head->rb_left);
	//printk(KERN_EMERG "parent: %ld, left: %lx, right: %lx",head->__rb_parent_color,head->rb_left,head->rb_right);
	//printk(KERN_EMERG "parent: %ld\n",head->__rb_parent_color);
	inorder(head->rb_right);
}
void init_mycfs_rq(struct mycfs_rq *mycfs_rq){
	//printk(KERN_EMERG "init mycfs_rq\n");
	mycfs_rq->root = MYRB_ROOT;
	//printk(KERN_EMERG "init mycfs_rq half\n");
	mycfs_rq->rb_leftmost = NULL;
	//printk(KERN_EMERG "init mycfs_rq done\n");
}
static struct sched_entity *__pick_next_entity(struct sched_entity *se){
	struct rb_node *next = rb_next(&se->run_node);
	struct sched_entity * re;
	if(!next){
		//printk(KERN_EMERG "__pick_next_entity NULL next!\n");
		//return NULL;
		return rb_entry(&se->run_node, struct  sched_entity, run_node);
	}
	//printk(KERN_EMERG "in __pick_next_entity next is %lx\n",next);
	re = rb_entry(next, struct  sched_entity, run_node);
	//printk(KERN_EMERG "in __pick_next_entity re is %lx\n",re);
	return re;
}
struct sched_entity *__pick_first_entity_mycfs(struct mycfs_rq *mycfs_rq)
{
	struct rb_node *left = mycfs_rq->rb_leftmost; //leftmost is the first
	struct sched_entity * se;
	if (!left){
		//printk("__pick_first_entity NULL left!\n");
		return NULL;
	}
	//printk(KERN_EMERG "__pick_first_entity_mycfs2\n");
	se = rb_entry(left, struct sched_entity, run_node);
	return se;
}
static inline void __update_curr(struct mycfs_rq *mycfs_rq, struct sched_entity *curr, unsigned long net_time){
		struct timeval prevtime;
		long long uelapsed = 0;
		schedstat_set(curr->statistics.exec_max,
		      max((u64)delta_exec, curr->statistics.exec_max));
		//float test = (float)
		prevtime.tv_sec = tv.tv_sec;
		prevtime.tv_usec = tv.tv_usec;
		do_gettimeofday(&tv);
		uelapsed = tv.tv_usec - prevtime.tv_usec + (tv.tv_sec - prevtime.tv_sec) * 1000000;
		curr-> timerunned += (unsigned long long)uelapsed;
		acctime += (unsigned long long)uelapsed;
		//printk(KERN_EMERG "before clear se %lx limit %ld\n",se,se->timelimit);
		if(acctime > 200000){// > 100ms
			acctime = 0;
			clear_runtime();
		}
		//if(mycfs_rq->curr->timerunned <= mycfs_rq->curr->timelimit){
			curr->sum_exec_runtime += (u64)net_time / 200000 * mycfs_rq->curr->timelimit;
		//}
		curr->vruntime += net_time;
	//}
}
static void update_curr(struct mycfs_rq *mycfs_rq){
	struct sched_entity *curr = mycfs_rq->curr;
	u64 now = myrq_of(mycfs_rq)->clock_task;
	unsigned long net_time;
	if(!curr){
		//printk(KERN_EMERG "in update_curr curr is null\n");
		return;
	}
	net_time = (unsigned long)(now - curr->exec_start);
	__update_curr(mycfs_rq, curr, net_time);
	curr->exec_start = now;
}
static void __enqueue_entity(struct mycfs_rq *mycfs_rq, struct sched_entity *se){
	struct rb_node **link = &mycfs_rq->root.rb_node;
	struct rb_node *parent = NULL;
	struct sched_entity *entry;
	struct task_struct *task;
	int leftmost = 1;
	int count=0;
	//printk(KERN_EMERG "in __enqueue_entity mycfs_rq = %lx, se = %lx\n",mycfs_rq,se);
	//printk(KERN_EMERG "before, root %lx\n",*link);
	while(*link) {//until reach end
		count++;
		parent = *link; 
		entry = rb_entry(parent, struct sched_entity, run_node);// find the sched entity of the rb_node
		task = task_of(entry);
		if(!entity_before(se, entry)){ //if the entity that we want to insert, is before the current pointed entity
			//printk(KERN_EMERG "pid %d goes to left\n",task->pid);
			link = &parent->rb_left; //then left
		}else{
			//printk(KERN_EMERG "pid %d goes to right\n",task->pid);
			link = &parent->rb_right; //otherwise right
			leftmost = 0;//not left most
		}
	}
	if(count == 0)
		mycfs_rq->root.rb_node = &se->run_node;
	if(leftmost==1)
		mycfs_rq->rb_leftmost = &se->run_node;
	//printk(KERN_EMERG "middle, rootnode %lx leftmost %lx\n",mycfs_rq->root.rb_node,mycfs_rq->rb_leftmost);
	rb_link_node(&se->run_node, parent, link);
	rb_insert_color(&se->run_node, &mycfs_rq->root);
	//inorder(mycfs_rq->root.rb_node);
	//printk(KERN_EMERG "after, rootnode %lx leftmost %lx\n",mycfs_rq->root.rb_node,mycfs_rq->rb_leftmost);
}
static void __dequeue_entity(struct mycfs_rq *mycfs_rq, struct sched_entity *se){
	if(mycfs_rq->rb_leftmost == &se->run_node){
		struct rb_node *next;
		next = rb_next(&se->run_node);
		mycfs_rq->rb_leftmost = next;
	}
	rb_erase(&se->run_node, &mycfs_rq->root);
}
static void entity_tick(struct mycfs_rq *mycfs_rq, struct sched_entity *curr, int queued){
	update_curr(mycfs_rq);
		if (queued) {
			resched_task(mycfs_rq_of(mycfs_rq)->curr);
			return;
		}
}
void enqueue_entity(struct mycfs_rq *mycfs_rq, struct sched_entity *se, int flags)
{
	struct task_struct *p = task_of(se);
	update_curr(mycfs_rq);
	if (se != mycfs_rq->curr){
		printk(KERN_EMERG "__enqueing pid %d\n",p->pid);
		__enqueue_entity(mycfs_rq, se);
	}
	//else
		//printk(KERN_EMERG "no need to enqueue, alraedy on q\n");
	se->on_rq = 1;
}
static void dequeue_entity(struct mycfs_rq *mycfs_rq, struct sched_entity *se, int flags)
{
	update_curr(mycfs_rq);
	if (se != mycfs_rq->curr)
		__dequeue_entity(mycfs_rq, se);
	//else
		//printk(KERN_EMERG "no need to dequeue, not on q\n");
	se->on_rq = 0;
}
static void put_prev_entity(struct mycfs_rq *mycfs_rq, struct sched_entity *prev){
	//printk(KERN_EMERG "put_prev_entity\n");
	if (prev->on_rq)
		update_curr(mycfs_rq);
	if(prev->on_rq){
		__enqueue_entity(mycfs_rq, prev);
	}
	mycfs_rq->curr = NULL;
	//printk(KERN_EMERG "put_prev_entity done\n");
}
static struct sched_entity *pick_next_entity(struct mycfs_rq *mycfs_rq){
	struct sched_entity *se;
	struct sched_entity *re;
	se = __pick_first_entity_mycfs(mycfs_rq);
	//if(!mycfs_rq)
	//	printk(KERN_EMERG "in pick_next_entity mycfs_rq is null\n");
	//if(!se)
	//	printk(KERN_EMERG "in pick_next_entity se is null");
	//if(!se->run_node)
		//printk(KERN_EMERG "in pick_next_entity se->runnode is null");
	re = __pick_next_entity(se);
	return re;
}
static void enqueue_task_mycfs(struct rq *rq, struct task_struct *p, int flags){
	struct mycfs_rq *mycfs_rq;
	struct sched_entity *se = &p->se;
	for_each_sched_entity(se){
		if(se->on_rq)
			break;
		mycfs_rq = mycfs_rq_of(se);
		//printk(KERN_EMERG "in enqueue_task_mycfs putting se %lx into mycfs_rq %lx\n",se,mycfs_rq);
		enqueue_entity(mycfs_rq, se, flags);
	}
}
static void dequeue_task_mycfs(struct rq *rq, struct task_struct *p, int flags)
{
	struct mycfs_rq *mycfs_rq;
	struct sched_entity *se = &p->se;
	for_each_sched_entity(se) {
		mycfs_rq = mycfs_rq_of(se);
		dequeue_entity(mycfs_rq, se, flags);
	}
}
static void set_next_entity(struct mycfs_rq *mycfs_rq, struct sched_entity *se){
	if(se->on_rq){
		__dequeue_entity(mycfs_rq,se);
	}
	mycfs_rq->curr = se;
}
static void set_curr_task_mycfs(struct rq *rq)
{
	struct sched_entity *se = &rq->curr->se;
	struct task_struct *task;
	int cpu;
	cpu = smp_processor_id();
	//printk(KERN_EMERG "set_curr_task_mycfs for pid %d. addr of rq %lx, cpu %d\n",rq->curr->pid,rq,cpu);
	for_each_sched_entity(se) {
		struct mycfs_rq *mycfs_rq = mycfs_rq_of(se);
		if(!mycfs_rq && not_init){
			printk(KERN_EMERG "creating mycfs_rq!\n");
			init_mycfs_rq(&rq->mycfs);
			not_init = 0;
		}
		task = task_of(se);
		//printk(KERN_EMERG "creating mycfs_rq done! addr %lx\n",&rq->mycfs);
		//set_next_entity(cfs_rq, se);
		if(rq->mycfs.curr)
			rq->mycfs.prev = rq->mycfs.curr;
		rq->mycfs.curr = se;
		rq->mycfs.rq = rq;
		se->mycfs_rq = &rq->mycfs;
		sys_sched_setlimit(100, task->pid);
		do_gettimeofday(&tv);
		/* ensure bandwidth has been allocated on our new cfs_rq */
	}
	//printk(KERN_EMERG "done set_curr\n");
}
static void put_prev_task_mycfs(struct rq *rq, struct task_struct *prev){
	
	struct sched_entity *se = &prev->se;
	struct mycfs_rq *mycfs_rq;
	//printk(KERN_EMERG "put_prev_task_mycfs\n");
	for_each_sched_entity(se){
		mycfs_rq = mycfs_rq_of(se);
		if(!mycfs_rq)
			printk(KERN_EMERG "null mycfs_rq\n");
		put_prev_entity(mycfs_rq,se);
	}
	//printk(KERN_EMERG "put_prev_task_mycfs done with pid %d\n",prev->pid);
}
static struct task_struct *pick_next_task_mycfs(struct rq *rq){
	
	struct task_struct *p,*q;
	struct mycfs_rq *mycfs_rq = &rq->mycfs;//......
	struct sched_entity *se;
	struct timeval prevtime;
	long long uelapsed = 0;
	if(not_init)
		return NULL;
	if(!mycfs_rq->root.rb_node){
		//printk(KERN_EMERG "in pick_next_task_mycfs, cfs_rq.root is null. addr of cfs_rq %lx, returning null\n",mycfs_rq);
		return NULL;
	}
	prevtime.tv_sec = tv.tv_sec;
	prevtime.tv_usec = tv.tv_usec;
	do_gettimeofday(&tv);
	uelapsed = tv.tv_usec - prevtime.tv_usec + (tv.tv_sec - prevtime.tv_sec) * 1000000;
	/*if(prevtime.tv_sec != 0 || prevtime.tv_usec != 0){
		printk(KERN_EMERG "prevtime %d sec %lld usec, %ld usec\n",prevtime.tv_sec,prevtime.tv_usec,prevtime.tv_usec);
		printk(KERN_EMERG "delta time %d sec %lld usec\n",tv.tv_sec - prevtime.tv_sec, tv.tv_usec - prevtime.tv_usec);
	}*/

	//printk(KERN_EMERG "pick_next_task_mycfs\n");
	//do {
		se = pick_next_entity(mycfs_rq);
		if(!se)
			printk(KERN_EMERG "null se\n");
		q = task_of(se);
		if(se->timerunned > se->timelimit ){
			//rq->curr = NULL;
			//printk(KERN_EMERG "pid %d exceeds time limit! time runned is %ld, time limit is %ld, addr of se %lx\n",q->pid, se->timerunned, se->timelimit, se);
			//printk(KERN_EMERG "pid %d exceeds time limit! addr of se %lx\n",q->pid,se);
			se-> timerunned += (unsigned long long)uelapsed;
			acctime += (unsigned long long)uelapsed;
			if(acctime > 200000){// > 100ms
				acctime = 0;
				clear_runtime();
			}
			//printk(KERN_EMERG "NULL returen\n");
			return NULL;
		}
		se-> timerunned += (unsigned long long)uelapsed;
		acctime += (unsigned long long)uelapsed;
		//printk(KERN_EMERG "before clear se %lx limit %ld\n",se,se->timelimit);
		if(acctime > 200000){// > 100ms
			acctime = 0;
			clear_runtime();
		}
		//printk(KERN_EMERG "after compare se %lx limit %ld\n",se,se->timelimit);
		set_next_entity(mycfs_rq, se);
	//	cfs_rq = group_cfs_rq(se);
	//} while (cfs_rq);
	//printk(KERN_EMERG "time elapsed %lld usec\n",(unsigned long long)uelapsed);
	p = task_of(se);
	//printk(KERN_EMERG "pick_next_task_mycfs done pid %d, time to run %ld, load weight %ld\n",p->pid,se->vruntime,se->load.weight);
	
	return p;
}

static void task_tick_mycfs(struct rq *rq, struct task_struct *curr, int queued){
	struct mycfs_rq *mycfs_rq;
	struct sched_entity *se = &curr->se;
	for_each_sched_entity(se) {
		mycfs_rq = mycfs_rq_of(se);
		entity_tick(mycfs_rq, se, queued);
	}
	return;
}
static void check_preempt_wakeup_mycfs(struct rq *rq, struct task_struct *p, int wake_flags){
	return;
}
static void prio_changed_mycfs(struct rq *rq, struct task_struct *p, int oldprio){
	return;
}
static void switched_to_mycfs(struct rq *rq, struct task_struct *p){
	return;
}
static void task_waking_mycfs(struct task_struct *p){
	return;
}
static void task_woken_mycfs(struct rq *rq, struct task_struct *p,long *flag){
	return;
}
static int select_task_rq_mycfs(struct task_struct *p, int sd_flag, int wake_flags)
{
	return 0;
}
static void task_fork_mycfs(struct task_struct *p){
	struct mycfs_rq *mycfs_rq;
	struct sched_entity *se = &p->se, *curr;
	struct rq *rq = cpu_rq(0);
	printk(KERN_EMERG "in task_fork_mycfs pid %d\n",p->pid);
	mycfs_rq = task_mycfs_rq(current);
	curr = mycfs_rq->curr;
	update_curr(mycfs_rq);
	if (curr)
		se->vruntime = curr->vruntime;
}
const struct sched_class mycfs_sched_class = {
	.next			= &idle_sched_class,
	.enqueue_task		= enqueue_task_mycfs,
	.dequeue_task		= dequeue_task_mycfs,
	//.yield_task		= yield_task_fair,
	//.yield_to_task		= yield_to_task_fair,
	.task_tick		= task_tick_mycfs,
	.check_preempt_curr	= check_preempt_wakeup_mycfs,
	.set_curr_task          = set_curr_task_mycfs,
	.pick_next_task		= pick_next_task_mycfs,
	.put_prev_task		= put_prev_task_mycfs,
	.prio_changed		= prio_changed_mycfs,
	.switched_to		= switched_to_mycfs,
	.task_waking		= task_waking_mycfs,
	.task_woken         = task_woken_mycfs,
	.select_task_rq		= select_task_rq_mycfs,
	.task_fork		= task_fork_mycfs,
};