/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/kfifo.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/notifier.h>
#include <net/genetlink.h>

#include "lfsm.h"

static const char * const link_state_str[] = {
    [LINK_DOWN]     = "LINK_DOWN",
    [LINK_STARTING] = "LINK_STARTING",
    [LINK_UP]       = "LINK_UP",
    [LINK_STOPPING] = "LINK_STOPPING",
};

static const char * const lfsm_action_str[] = {
    [LFSM_ACT_LINK_UP]   = "LINK_UP",
    [LFSM_ACT_LINK_DOWN] = "LINK_DOWN",
};

struct lfsm_action {
    enum lfsm_action_type type;
    void *context; /* For future */
};

DEFINE_KFIFO(lfsm_queue, struct lfsm_action, 16);
static enum link_state link_lfsm_state = LINK_DOWN;
static DEFINE_SPINLOCK(lfsm_lock);

static struct workqueue_struct *lfsm_wq;
static struct work_struct lfsm_worker;
static struct work_struct lfsm_up_work;
static struct work_struct lfsm_down_work;
static struct delayed_work lfsm_timeout_work;
static bool lfsm_work_active = false;
static struct kobject *lfsm_kobj;

/* Notifier chain */
static BLOCKING_NOTIFIER_HEAD(link_state_notifier_chain);

int lfsm_register_link_state_notifier(struct notifier_block *nb) {
    return blocking_notifier_chain_register(&link_state_notifier_chain, nb);
}
EXPORT_SYMBOL_GPL(lfsm_register_link_state_notifier);

int lfsm_unregister_link_state_notifier(struct notifier_block *nb) {
    return blocking_notifier_chain_unregister(&link_state_notifier_chain, nb);
}
EXPORT_SYMBOL_GPL(lfsm_unregister_link_state_notifier);

/* Generic Netlink Definitions */
static struct genl_family lfsm_genl_family;

enum {
    LFSM_ATTR_UNSPEC,
    LFSM_ATTR_LINK_STATE,
    __LFSM_ATTR_MAX,
};
#define LFSM_ATTR_MAX (__LFSM_ATTR_MAX - 1)

static struct nla_policy lfsm_nl_policy[LFSM_ATTR_MAX + 1] = {
    [LFSM_ATTR_LINK_STATE] = { .type = NLA_U32 },
};

enum {
    LFSM_CMD_UNSPEC,
    LFSM_CMD_NOTIFY,
    LFSM_CMD_LINK_UP,
    LFSM_CMD_LINK_DOWN,
    LFSM_CMD_CANCEL,
    __LFSM_CMD_MAX,
};
#define LFSM_CMD_MAX (__LFSM_CMD_MAX - 1)

enum {
    LFSM_MCGRP_EVENTS,
};

static const struct genl_multicast_group lfsm_mcgrps[] = {
    [LFSM_MCGRP_EVENTS] = { .name = "lfsm_events" },
};

static void lfsm_notify_state(enum link_state state) {
    struct sk_buff *skb;
    void *msg_head;

    skb = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
    if (!skb)
        return;

    msg_head = genlmsg_put(skb, 0, 0, &lfsm_genl_family, 0, LFSM_CMD_NOTIFY);
    if (!msg_head)
        goto free;

    if (nla_put_u32(skb, LFSM_ATTR_LINK_STATE, state))
        goto free;

    genlmsg_end(skb, msg_head);
    genlmsg_multicast(&lfsm_genl_family, skb, 0, LFSM_MCGRP_EVENTS, GFP_KERNEL);
    return;
free:
    nlmsg_free(skb);
}

static int lfsm_cmd_handler(struct sk_buff *skb, struct genl_info *info) {
    if (!info)
        return -EINVAL;

    switch (info->genlhdr->cmd) {
    case LFSM_CMD_LINK_UP:
        return lfsm_link_up();
    case LFSM_CMD_LINK_DOWN:
        return lfsm_link_down();
    case LFSM_CMD_CANCEL:
        lfsm_force_down();
        return 0;
    default:
        return -EOPNOTSUPP;
    }
}

static const struct genl_ops lfsm_genl_ops[] = {
    {
        .cmd = LFSM_CMD_LINK_UP,
        .doit = lfsm_cmd_handler,
        .policy = lfsm_nl_policy,
        .flags = GENL_ADMIN_PERM,
    },
    {
        .cmd = LFSM_CMD_LINK_DOWN,
        .doit = lfsm_cmd_handler,
        .policy = lfsm_nl_policy,
        .flags = GENL_ADMIN_PERM,
    },
    {
        .cmd = LFSM_CMD_CANCEL,
        .doit = lfsm_cmd_handler,
        .policy = lfsm_nl_policy,
        .flags = GENL_ADMIN_PERM,
    }
};

static struct genl_family lfsm_genl_family = {
    .name = "lfsm_notify",
    .version = 1,
    .maxattr = LFSM_ATTR_MAX,
    .module = THIS_MODULE,
    .ops = lfsm_genl_ops,
    .n_ops = ARRAY_SIZE(lfsm_genl_ops),
    .mcgrps = lfsm_mcgrps,
    .n_mcgrps = ARRAY_SIZE(lfsm_mcgrps),
};

static void lfsm_timeout_worker(struct work_struct *work)
{
    spin_lock(&lfsm_lock);
    pr_warn("LFSM: Transition timed out. Forcing link DOWN\n");

    cancel_work_sync(&lfsm_up_work);
    cancel_work_sync(&lfsm_down_work);
    kfifo_reset(&lfsm_queue);

    link_lfsm_state = LINK_DOWN;
    lfsm_work_active = false;
    spin_unlock(&lfsm_lock);
}

// --- Transition Workers ---
static void lfsm_up_worker(struct work_struct *work)
{
    msleep(LFSM_DELAY_MS);

    spin_lock(&lfsm_lock);
    cancel_delayed_work_sync(&lfsm_timeout_work);
    link_lfsm_state = LINK_UP;
    pr_info("LFSM: Link is UP\n");
    queue_work(lfsm_wq, &lfsm_worker);
    spin_unlock(&lfsm_lock);

    lfsm_notify_state(LINK_UP);
    blocking_notifier_call_chain(&link_state_notifier_chain, LINK_UP, NULL);
}

static void lfsm_down_worker(struct work_struct *work)
{
    msleep(LFSM_DELAY_MS);

    spin_lock(&lfsm_lock);
    cancel_delayed_work_sync(&lfsm_timeout_work);
    link_lfsm_state = LINK_DOWN;
    pr_info("LFSM: Link is DOWN\n");
    queue_work(lfsm_wq, &lfsm_worker);
    spin_unlock(&lfsm_lock);

    lfsm_notify_state(LINK_DOWN);
    blocking_notifier_call_chain(&link_state_notifier_chain, LINK_DOWN, NULL);
}

// --- LFSM Dispatcher ---
static void lfsm_dispatch_worker(struct work_struct *work)
{
    struct lfsm_action act;

    spin_lock(&lfsm_lock);
    if (!kfifo_out(&lfsm_queue, &act, 1)) {
        lfsm_work_active = false;
        spin_unlock(&lfsm_lock);
        return;
    }

    switch (act.type) {
    case LFSM_ACT_LINK_UP:
        link_lfsm_state = LINK_STARTING;
        queue_delayed_work(lfsm_wq, &lfsm_timeout_work, msecs_to_jiffies(LFSM_TIMEOUT_MS));
        queue_work(lfsm_wq, &lfsm_up_work);
        break;

    case LFSM_ACT_LINK_DOWN:
        link_lfsm_state = LINK_STOPPING;
        queue_delayed_work(lfsm_wq, &lfsm_timeout_work, msecs_to_jiffies(LFSM_TIMEOUT_MS));
        queue_work(lfsm_wq, &lfsm_down_work);
        break;

    default:
        pr_warn("LFSM: Unknown action type %d\n", act.type);
        lfsm_work_active = false;
        break;
    }

    spin_unlock(&lfsm_lock);
}

// --- Public Link LFSM Wrapper API ---

static int enqueue_lfsm_action(enum lfsm_action_type type)
{
    struct lfsm_action act = { .type = type, .context = NULL };
    unsigned long flags;
    int ret = 0;

    spin_lock_irqsave(&lfsm_lock, flags);
    if (!kfifo_in(&lfsm_queue, &act, 1)) {
        ret = -ENOSPC;
        goto out;
    }

    if (!lfsm_work_active) {
        lfsm_work_active = true;
        queue_work(lfsm_wq, &lfsm_worker);
    }

    pr_info("LFSM: Queued action: %s\n", lfsm_action_str[type]);
out:
    spin_unlock_irqrestore(&lfsm_lock, flags);
    return ret;
}

/**
 * lfsm_link_up - Establishes a link or connection.
 *
 * This function is responsible for initializing or bringing up a link.
 * The specific behavior depends on the implementation details within
 * the function body.
 *
 * Return: 0 on success, negative error code on failure.
 */
int lfsm_link_up(void)
{
    unsigned long flags;
    int ret;

    spin_lock_irqsave(&lfsm_lock, flags);
    if (link_lfsm_state == LINK_DOWN)
        ret = enqueue_lfsm_action(LFSM_ACT_LINK_UP);
    else if (link_lfsm_state == LINK_UP)
        ret = 0;
    else
        ret = -EBUSY;
    spin_unlock_irqrestore(&lfsm_lock, flags);
    return ret;
}
EXPORT_SYMBOL_GPL(lfsm_link_up);

/**
 * lfsm_link_down - Handles the event when a link goes down.
 *
 * This function is called to perform necessary operations when the link
 * status changes to down. It may include cleanup, resource deallocation,
 * or notifying other subsystems about the link status change.
 *
 * Return: 0 on success, negative error code on failure.
 */
int lfsm_link_down(void)
{
    unsigned long flags;
    int ret;

    spin_lock_irqsave(&lfsm_lock, flags);
    if (link_lfsm_state == LINK_UP) {
        ret = enqueue_lfsm_action(LFSM_ACT_LINK_DOWN);
    } else if (link_lfsm_state == LINK_DOWN) {
        ret = 0;
    } else {
        ret = -EBUSY;
    }
    spin_unlock_irqrestore(&lfsm_lock, flags);
    return ret;
}
EXPORT_SYMBOL_GPL(lfsm_link_down);

/**
 * lfsm_get_link_state - Retrieve the current state of the link.
 *
 * This function returns the current state of the link as an enum link_state value.
 *
 * Return: The current link state.
 */
enum link_state lfsm_get_link_state(void)
{
    unsigned long flags;
    enum link_state st;

    spin_lock_irqsave(&lfsm_lock, flags);
    st = link_lfsm_state;
    spin_unlock_irqrestore(&lfsm_lock, flags);

    return st;
}
EXPORT_SYMBOL_GPL(lfsm_get_link_state);

/**
 * lfsm_force_down - Forcefully shuts down the LFSM subsystem.
 *
 * This function initiates a forced shutdown of the lfsm subsystem. It 
 * is typically used in scenarios where a graceful shutdown is not possible
 * or when an emergency stop is required to prevent further operations.
 *
 * Context: May be called in critical error handling paths.
 */
void lfsm_force_down(void)
{
    cancel_work_sync(&lfsm_worker);
    cancel_work_sync(&lfsm_up_work);
    cancel_work_sync(&lfsm_down_work);
    cancel_delayed_work_sync(&lfsm_timeout_work);

    spin_lock(&lfsm_lock);
    kfifo_reset(&lfsm_queue);
    link_lfsm_state = LINK_DOWN;
    lfsm_work_active = false;
    pr_info("LFSM: Cancelled all and forced link DOWN\n");
    spin_unlock(&lfsm_lock);
}
EXPORT_SYMBOL_GPL(lfsm_force_down);

// --- sysfs ---
static ssize_t state_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    enum link_state s = lfsm_get_link_state();
    return sprintf(buf, "%s\n", (s < LINK_STATE_MAX) ? link_state_str[s] : "UNKNOWN");
}
static struct kobj_attribute state_attr = __ATTR_RO(state);

static ssize_t queue_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    struct lfsm_action q[16];
    ssize_t len = 0;
    int i, n;
    unsigned long flags;

    spin_lock_irqsave(&lfsm_lock, flags);
    n = kfifo_out_peek(&lfsm_queue, q, ARRAY_SIZE(q));
    for (i = 0; i < n; i++) {
        if (q[i].type < LFSM_ACT_MAX)
            len += scnprintf(buf + len, PAGE_SIZE - len, "%s\n", lfsm_action_str[q[i].type]);
    }
    spin_unlock_irqrestore(&lfsm_lock, flags);
    return len;
}
static struct kobj_attribute queue_attr = __ATTR_RO(queue);

static struct attribute *lfsm_attrs[] = {
    &state_attr.attr,
    &queue_attr.attr,
    NULL,
};

static struct attribute_group lfsm_attr_group = {
    .attrs = lfsm_attrs,
};

// --- Init / Cleanup ---
static int __init lfsm_module_init(void)
{
    int ret;

    spin_lock_init(&lfsm_lock);
    INIT_WORK(&lfsm_worker, lfsm_dispatch_worker);
    INIT_WORK(&lfsm_up_work, lfsm_up_worker);
    INIT_WORK(&lfsm_down_work, lfsm_down_worker);
    INIT_DELAYED_WORK(&lfsm_timeout_work, lfsm_timeout_worker);

    lfsm_wq = alloc_workqueue("lfsm_wq", WQ_UNBOUND, 0);
    if (!lfsm_wq)
        return -ENOMEM;

    lfsm_kobj = kobject_create_and_add("lfsm", kernel_kobj);
    if (!lfsm_kobj) {
        ret = -ENOMEM;
        goto destroy_wq;
    }

    ret = sysfs_create_group(lfsm_kobj, &lfsm_attr_group);
    if (ret) {
        goto out;
    }

    pr_info("LFSM: Module loaded with generic action support.\n");

    return ret;

destroy_wq:
    destroy_workqueue(lfsm_wq);

out:
    destroy_workqueue(lfsm_wq);
    kobject_put(lfsm_kobj);

    return ret;
}

static void __exit lfsm_module_exit(void)
{
    lfsm_force_down();
    sysfs_remove_group(lfsm_kobj, &lfsm_attr_group);
    kobject_put(lfsm_kobj);
    destroy_workqueue(lfsm_wq);
    pr_info("LFSM: Module unloaded.\n");
}

module_init(lfsm_module_init);
module_exit(lfsm_module_exit);

MODULE_AUTHOR("Christopher Denny <christopherscottdenny@gmail.com>");
MODULE_DESCRIPTION("LFSM: Link Finite State Machine Kernel Module");
MODULE_LICENSE("GPL");
MODULE_ALIAS("lfsm");
