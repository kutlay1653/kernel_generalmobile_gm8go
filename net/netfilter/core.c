/* netfilter.c: look after the filters for various protocols.
 * Heavily influenced by the old firewall.c by David Bonn and Alan Cox.
 *
 * Thanks to Rob `CmdrTaco' Malda for not influencing this code in any
 * way.
 *
 * Rusty Russell (C)2000 -- This code is GPL.
 * Patrick McHardy (c) 2006-2012
 */
#include <linux/kernel.h>
#include <linux/netfilter.h>
#include <net/protocol.h>
#include <linux/init.h>
#include <linux/skbuff.h>
#include <linux/wait.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/if.h>
#include <linux/netdevice.h>
#include <linux/netfilter_ipv6.h>
#include <linux/inetdevice.h>
#include <linux/proc_fs.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <net/net_namespace.h>
#include <net/sock.h>

#include "nf_internals.h"

#ifdef CONFIG_MTK_NET_LOGGING
#include <linux/timer.h>
#include <linux/string.h>
#include <net/netfilter/nf_log.h>
#endif
static DEFINE_MUTEX(afinfo_mutex);

#ifdef CONFIG_MTK_NET_LOGGING

/* 2016/8/18:
  *			  1. Fix Bug for debug drop packet message!
  *			  2. Use local_clock() to record drop time!
  */
#define IPTABLES_DROP_PACKET_STATICS 10
/*Trace 500 packets*/
#define IPTABLES_DROP_PACKET_NUM  500
#define S_SIZE (1024 - (sizeof(unsigned int) + 1))

struct drop_packet_info_t {
	unsigned int packet_num;
	unsigned int len;
	u8 pf;
	unsigned int hook;
	u64 drop_time;
	char iface[IFNAMSIZ];
	nf_hookfn *table;
	int family;
	int already_print;
};

struct drop_packets_statics_t {
	long cnt;
	struct timer_list print_drop_packets_timer;
	unsigned long print_stamp;
	unsigned int print_len;
	struct drop_packet_info_t drop_packets[IPTABLES_DROP_PACKET_NUM];
};

struct drop_packets_statics_t iptables_drop_packets;
struct sbuff {
	unsigned int	count;
	char		buf[S_SIZE + 1];
};

static __printf(2, 3) int sb_add(struct sbuff *m, const char *f, ...)
{
	va_list args;
	int len;

	if (likely(m->count < S_SIZE)) {
		va_start(args, f);
		len = vsnprintf(m->buf + m->count, S_SIZE - m->count, f, args);
		va_end(args);
		if (likely(m->count + len < S_SIZE)) {
			m->count += len;
			return 0;
			}
		}
	m->count = S_SIZE;
	return -1;
}

static size_t drop_time(u64 ts, char *buf)
{
	unsigned long rem_nsec;

	rem_nsec = do_div(ts, 1000000000);
	return sprintf(buf, "%5lu.%06lu",
			   (unsigned long)ts, rem_nsec / 1000);
}

static void iptables_drop_packet_monitor(unsigned long data)
{
	int limit = 0;
	int i = 0;
	int size = 0;
	int num  = iptables_drop_packets.cnt % 500;
	struct sbuff m;
	char time_buff[30];

	m.count = 0;
	memset(m.buf, 0, sizeof(m.buf));
	memset(time_buff, 0, sizeof(time_buff));
	if (num > 10)
		limit = 10;
	else
		limit = num;
	if ((iptables_drop_packets.print_len + limit) > num)
		size = num;
	else
		size = iptables_drop_packets.print_len + limit;
	for (i = iptables_drop_packets.print_len; i < size; i++) {
		if (iptables_drop_packets.drop_packets[i].already_print == 0) {
			sb_add(&m, "[mtk_net][drop](%ld,%d),",
			       iptables_drop_packets.cnt,
				iptables_drop_packets.drop_packets[i].packet_num);
			switch (iptables_drop_packets.drop_packets[i].pf) {
			case NFPROTO_IPV4:
				sb_add(&m, "[IPV4],");
				break;
			case NFPROTO_IPV6:
				sb_add(&m, "[IPV6],");
				break;
			case NFPROTO_ARP:
				sb_add(&m, "[ARP],");
				break;
			default:
				sb_add(&m, "[UNSPEC],");
				break;
			}
			switch (iptables_drop_packets.drop_packets[i].hook) {
			case NF_INET_PRE_ROUTING:
				sb_add(&m, "[PRE_ROUTING],");
				break;
			case NF_INET_LOCAL_IN:
				sb_add(&m, "[LOCAL_IN],");
				break;
			case NF_INET_FORWARD:
				sb_add(&m, "[FORWARD],");
				break;
			case NF_INET_LOCAL_OUT:
				sb_add(&m, "[LOCAL_OUT],");
				break;
			case NF_INET_POST_ROUTING:
				sb_add(&m, "[POST_ROUTING],");
				break;
			default:
				sb_add(&m, "[Unspec chain],");
				break;
			}

			drop_time(iptables_drop_packets.drop_packets[i].drop_time, time_buff);
			sb_add(&m, "[%pS],[%s],[%s]", iptables_drop_packets.drop_packets[i].table,
			       iptables_drop_packets.drop_packets[i].iface, time_buff);
			pr_info("%s\n", m.buf);
			m.count = 0;
			memset(m.buf, 0, sizeof(m.buf));
			memset(time_buff, 0, sizeof(time_buff));
			iptables_drop_packets.drop_packets[i].already_print = 1;
		}
	}
iptables_drop_packets.print_len = (iptables_drop_packets.print_len + limit) % 500;
}
#endif

const struct nf_afinfo __rcu *nf_afinfo[NFPROTO_NUMPROTO] __read_mostly;
EXPORT_SYMBOL(nf_afinfo);
const struct nf_ipv6_ops __rcu *nf_ipv6_ops __read_mostly;
EXPORT_SYMBOL_GPL(nf_ipv6_ops);

DEFINE_PER_CPU(bool, nf_skb_duplicated);
EXPORT_SYMBOL_GPL(nf_skb_duplicated);

int nf_register_afinfo(const struct nf_afinfo *afinfo)
{
	mutex_lock(&afinfo_mutex);
	RCU_INIT_POINTER(nf_afinfo[afinfo->family], afinfo);
	mutex_unlock(&afinfo_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(nf_register_afinfo);

void nf_unregister_afinfo(const struct nf_afinfo *afinfo)
{
	mutex_lock(&afinfo_mutex);
	RCU_INIT_POINTER(nf_afinfo[afinfo->family], NULL);
	mutex_unlock(&afinfo_mutex);
	synchronize_rcu();
}
EXPORT_SYMBOL_GPL(nf_unregister_afinfo);

#ifdef HAVE_JUMP_LABEL
struct static_key nf_hooks_needed[NFPROTO_NUMPROTO][NF_MAX_HOOKS];
EXPORT_SYMBOL(nf_hooks_needed);
#endif

static DEFINE_MUTEX(nf_hook_mutex);

static struct list_head *nf_find_hook_list(struct net *net,
					   const struct nf_hook_ops *reg)
{
	struct list_head *hook_list = NULL;

	if (reg->pf != NFPROTO_NETDEV)
		hook_list = &net->nf.hooks[reg->pf][reg->hooknum];
	else if (reg->hooknum == NF_NETDEV_INGRESS) {
#ifdef CONFIG_NETFILTER_INGRESS
		if (reg->dev && dev_net(reg->dev) == net)
			hook_list = &reg->dev->nf_hooks_ingress;
#endif
	}
	return hook_list;
}

struct nf_hook_entry {
	const struct nf_hook_ops	*orig_ops;
	struct nf_hook_ops		ops;
};

int nf_register_net_hook(struct net *net, const struct nf_hook_ops *reg)
{
	struct list_head *hook_list;
	struct nf_hook_entry *entry;
	struct nf_hook_ops *elem;

	entry = kmalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->orig_ops	= reg;
	entry->ops	= *reg;

	hook_list = nf_find_hook_list(net, reg);
	if (!hook_list) {
		kfree(entry);
		return -ENOENT;
	}

	mutex_lock(&nf_hook_mutex);
	list_for_each_entry(elem, hook_list, list) {
		if (reg->priority < elem->priority)
			break;
	}
	list_add_rcu(&entry->ops.list, elem->list.prev);
	mutex_unlock(&nf_hook_mutex);
#ifdef CONFIG_NETFILTER_INGRESS
	if (reg->pf == NFPROTO_NETDEV && reg->hooknum == NF_NETDEV_INGRESS)
		net_inc_ingress_queue();
#endif
#ifdef HAVE_JUMP_LABEL
	static_key_slow_inc(&nf_hooks_needed[reg->pf][reg->hooknum]);
#endif
	return 0;
}
EXPORT_SYMBOL(nf_register_net_hook);

void nf_unregister_net_hook(struct net *net, const struct nf_hook_ops *reg)
{
	struct list_head *hook_list;
	struct nf_hook_entry *entry;
	struct nf_hook_ops *elem;

	hook_list = nf_find_hook_list(net, reg);
	if (!hook_list)
		return;

	mutex_lock(&nf_hook_mutex);
	list_for_each_entry(elem, hook_list, list) {
		entry = container_of(elem, struct nf_hook_entry, ops);
		if (entry->orig_ops == reg) {
			list_del_rcu(&entry->ops.list);
			break;
		}
	}
	mutex_unlock(&nf_hook_mutex);
	if (&elem->list == hook_list) {
		WARN(1, "nf_unregister_net_hook: hook not found!\n");
		return;
	}
#ifdef CONFIG_NETFILTER_INGRESS
	if (reg->pf == NFPROTO_NETDEV && reg->hooknum == NF_NETDEV_INGRESS)
		net_dec_ingress_queue();
#endif
#ifdef HAVE_JUMP_LABEL
	static_key_slow_dec(&nf_hooks_needed[reg->pf][reg->hooknum]);
#endif
	synchronize_net();
	nf_queue_nf_hook_drop(net, &entry->ops);
	/* other cpu might still process nfqueue verdict that used reg */
	synchronize_net();
	kfree(entry);
}
EXPORT_SYMBOL(nf_unregister_net_hook);

int nf_register_net_hooks(struct net *net, const struct nf_hook_ops *reg,
			  unsigned int n)
{
	unsigned int i;
	int err = 0;

	for (i = 0; i < n; i++) {
		err = nf_register_net_hook(net, &reg[i]);
		if (err)
			goto err;
	}
	return err;

err:
	if (i > 0)
		nf_unregister_net_hooks(net, reg, i);
	return err;
}
EXPORT_SYMBOL(nf_register_net_hooks);

void nf_unregister_net_hooks(struct net *net, const struct nf_hook_ops *reg,
			     unsigned int n)
{
	while (n-- > 0)
		nf_unregister_net_hook(net, &reg[n]);
}
EXPORT_SYMBOL(nf_unregister_net_hooks);

static LIST_HEAD(nf_hook_list);

int nf_register_hook(struct nf_hook_ops *reg)
{
	struct net *net, *last;
	int ret;

	rtnl_lock();
	for_each_net(net) {
		ret = nf_register_net_hook(net, reg);
		if (ret && ret != -ENOENT)
			goto rollback;
	}
	list_add_tail(&reg->list, &nf_hook_list);
	rtnl_unlock();

	return 0;
rollback:
	last = net;
	for_each_net(net) {
		if (net == last)
			break;
		nf_unregister_net_hook(net, reg);
	}
	rtnl_unlock();
	return ret;
}
EXPORT_SYMBOL(nf_register_hook);

void nf_unregister_hook(struct nf_hook_ops *reg)
{
	struct net *net;

	rtnl_lock();
	list_del(&reg->list);
	for_each_net(net)
		nf_unregister_net_hook(net, reg);
	rtnl_unlock();
}
EXPORT_SYMBOL(nf_unregister_hook);

int nf_register_hooks(struct nf_hook_ops *reg, unsigned int n)
{
	unsigned int i;
	int err = 0;

	for (i = 0; i < n; i++) {
		err = nf_register_hook(&reg[i]);
		if (err)
			goto err;
	}
	return err;

err:
	if (i > 0)
		nf_unregister_hooks(reg, i);
	return err;
}
EXPORT_SYMBOL(nf_register_hooks);

void nf_unregister_hooks(struct nf_hook_ops *reg, unsigned int n)
{
	while (n-- > 0)
		nf_unregister_hook(&reg[n]);
}
EXPORT_SYMBOL(nf_unregister_hooks);

unsigned int nf_iterate(struct list_head *head,
			struct sk_buff *skb,
			struct nf_hook_state *state,
			struct nf_hook_ops **elemp)
{
	unsigned int verdict;

	/*
	 * The caller must not block between calls to this
	 * function because of risk of continuing from deleted element.
	 */
	list_for_each_entry_continue_rcu((*elemp), head, list) {
		if (state->thresh > (*elemp)->priority)
			continue;

		/* Optimization: we don't need to hold module
		   reference here, since function can't sleep. --RR */
repeat:
		verdict = (*elemp)->hook((*elemp)->priv, skb, state);
		if (verdict != NF_ACCEPT) {
#ifdef CONFIG_NETFILTER_DEBUG
			if (unlikely((verdict & NF_VERDICT_MASK)
							> NF_MAX_VERDICT)) {
				NFDEBUG("Evil return from %p(%u).\n",
					(*elemp)->hook, state->hook);
				continue;
			}
#endif
			if (verdict != NF_REPEAT)
				return verdict;
			goto repeat;
		}
	}
	return NF_ACCEPT;
}


/* Returns 1 if okfn() needs to be executed by the caller,
 * -EPERM for NF_DROP, 0 otherwise. */
int nf_hook_slow(struct sk_buff *skb, struct nf_hook_state *state)
{
	struct nf_hook_ops *elem;
	unsigned int verdict;
	int ret = 0;
#ifdef CONFIG_MTK_NET_LOGGING
	unsigned int num;
#endif
	/* We may already have this, but read-locks nest anyway */
	rcu_read_lock();

	elem = list_entry_rcu(state->hook_list, struct nf_hook_ops, list);
next_hook:
	verdict = nf_iterate(state->hook_list, skb, state, &elem);
	if (verdict == NF_ACCEPT || verdict == NF_STOP) {
		ret = 1;
	} else if ((verdict & NF_VERDICT_MASK) == NF_DROP) {
#ifdef CONFIG_MTK_NET_LOGGING
	/*because skb free  need copy some info to ...*/
	if (iptables_drop_packets.cnt > 100000)
		iptables_drop_packets.cnt = 1;
	num = iptables_drop_packets.cnt % 500;
	iptables_drop_packets.drop_packets[num].drop_time = local_clock();
	iptables_drop_packets.drop_packets[num].len = skb->len;
	iptables_drop_packets.drop_packets[num].hook = state->hook;
	iptables_drop_packets.drop_packets[num].pf = state->pf;
	iptables_drop_packets.drop_packets[num].table = elem->hook;
	iptables_drop_packets.drop_packets[num].packet_num = num + 1;
	iptables_drop_packets.drop_packets[num].already_print = 0;
	if (skb->dev && skb->dev->name) {
		strncpy(iptables_drop_packets.drop_packets[num].iface, skb->dev->name, IFNAMSIZ);
		iptables_drop_packets.drop_packets[num].iface[IFNAMSIZ - 1] = 0;
	}
	iptables_drop_packets.cnt++;
	if ((jiffies - iptables_drop_packets.print_stamp) / HZ > IPTABLES_DROP_PACKET_STATICS) {
		iptables_drop_packets.print_stamp = jiffies;
		mod_timer(&iptables_drop_packets.print_drop_packets_timer, jiffies);
	}
#endif

		kfree_skb(skb);
		ret = NF_DROP_GETERR(verdict);
		if (ret == 0)
			ret = -EPERM;
	} else if ((verdict & NF_VERDICT_MASK) == NF_QUEUE) {
		int err = nf_queue(skb, elem, state,
				   verdict >> NF_VERDICT_QBITS);
		if (err < 0) {
			if (err == -ESRCH &&
			   (verdict & NF_VERDICT_FLAG_QUEUE_BYPASS))
				goto next_hook;
			kfree_skb(skb);
		}
	}
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL(nf_hook_slow);


int skb_make_writable(struct sk_buff *skb, unsigned int writable_len)
{
	if (writable_len > skb->len)
		return 0;

	/* Not exclusive use of packet?  Must copy. */
	if (!skb_cloned(skb)) {
		if (writable_len <= skb_headlen(skb))
			return 1;
	} else if (skb_clone_writable(skb, writable_len))
		return 1;

	if (writable_len <= skb_headlen(skb))
		writable_len = 0;
	else
		writable_len -= skb_headlen(skb);

	return !!__pskb_pull_tail(skb, writable_len);
}
EXPORT_SYMBOL(skb_make_writable);

/* This needs to be compiled in any case to avoid dependencies between the
 * nfnetlink_queue code and nf_conntrack.
 */
struct nfnl_ct_hook __rcu *nfnl_ct_hook __read_mostly;
EXPORT_SYMBOL_GPL(nfnl_ct_hook);

#if IS_ENABLED(CONFIG_NF_CONNTRACK)
/* This does not belong here, but locally generated errors need it if connection
   tracking in use: without this, connection may not be in hash table, and hence
   manufactured ICMP or RST packets will not be associated with it. */
void (*ip_ct_attach)(struct sk_buff *, const struct sk_buff *)
		__rcu __read_mostly;
EXPORT_SYMBOL(ip_ct_attach);

void nf_ct_attach(struct sk_buff *new, const struct sk_buff *skb)
{
	void (*attach)(struct sk_buff *, const struct sk_buff *);

	if (skb->nfct) {
		rcu_read_lock();
		attach = rcu_dereference(ip_ct_attach);
		if (attach)
			attach(new, skb);
		rcu_read_unlock();
	}
}
EXPORT_SYMBOL(nf_ct_attach);

void (*nf_ct_destroy)(struct nf_conntrack *) __rcu __read_mostly;
EXPORT_SYMBOL(nf_ct_destroy);

void nf_conntrack_destroy(struct nf_conntrack *nfct)
{
	void (*destroy)(struct nf_conntrack *);

	rcu_read_lock();
	destroy = rcu_dereference(nf_ct_destroy);
	BUG_ON(destroy == NULL);
	destroy(nfct);
	rcu_read_unlock();
}
EXPORT_SYMBOL(nf_conntrack_destroy);

/* Built-in default zone used e.g. by modules. */
const struct nf_conntrack_zone nf_ct_zone_dflt = {
	.id	= NF_CT_DEFAULT_ZONE_ID,
	.dir	= NF_CT_DEFAULT_ZONE_DIR,
};
EXPORT_SYMBOL_GPL(nf_ct_zone_dflt);
#endif /* CONFIG_NF_CONNTRACK */

#ifdef CONFIG_NF_NAT_NEEDED
void (*nf_nat_decode_session_hook)(struct sk_buff *, struct flowi *);
EXPORT_SYMBOL(nf_nat_decode_session_hook);
#endif

static int nf_register_hook_list(struct net *net)
{
	struct nf_hook_ops *elem;
	int ret;

	rtnl_lock();
	list_for_each_entry(elem, &nf_hook_list, list) {
		ret = nf_register_net_hook(net, elem);
		if (ret && ret != -ENOENT)
			goto out_undo;
	}
	rtnl_unlock();
	return 0;

out_undo:
	list_for_each_entry_continue_reverse(elem, &nf_hook_list, list)
		nf_unregister_net_hook(net, elem);
	rtnl_unlock();
	return ret;
}

static void nf_unregister_hook_list(struct net *net)
{
	struct nf_hook_ops *elem;

	rtnl_lock();
	list_for_each_entry(elem, &nf_hook_list, list)
		nf_unregister_net_hook(net, elem);
	rtnl_unlock();
}

static int __net_init netfilter_net_init(struct net *net)
{
	int i, h, ret;

	for (i = 0; i < ARRAY_SIZE(net->nf.hooks); i++) {
		for (h = 0; h < NF_MAX_HOOKS; h++)
			INIT_LIST_HEAD(&net->nf.hooks[i][h]);
	}

#ifdef CONFIG_PROC_FS
	net->nf.proc_netfilter = proc_net_mkdir(net, "netfilter",
						net->proc_net);
	if (!net->nf.proc_netfilter) {
		if (!net_eq(net, &init_net))
			pr_err("cannot create netfilter proc entry");

		return -ENOMEM;
	}
#endif
	ret = nf_register_hook_list(net);
	if (ret)
		remove_proc_entry("netfilter", net->proc_net);

#ifdef CONFIG_MTK_NET_LOGGING
	init_timer(&iptables_drop_packets.print_drop_packets_timer);
	iptables_drop_packets.print_drop_packets_timer.function = iptables_drop_packet_monitor;
#endif
	return ret;
}

static void __net_exit netfilter_net_exit(struct net *net)
{
	nf_unregister_hook_list(net);
	remove_proc_entry("netfilter", net->proc_net);
}

static struct pernet_operations netfilter_net_ops = {
	.init = netfilter_net_init,
	.exit = netfilter_net_exit,
};

int __init netfilter_init(void)
{
	int ret;

	ret = register_pernet_subsys(&netfilter_net_ops);
	if (ret < 0)
		goto err;

	ret = netfilter_log_init();
	if (ret < 0)
		goto err_pernet;

	return 0;
err_pernet:
	unregister_pernet_subsys(&netfilter_net_ops);
err:
	return ret;
}