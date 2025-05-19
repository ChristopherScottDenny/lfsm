#ifndef LSFM_H
#define LSFM_H

#define LSFM_MODULE_NAME "lsfm"
#define LFSM_DELAY_MS 1000
#define LFSM_TIMEOUT_MS (3 * LFSM_DELAY_MS)

struct notifier_block;

/* LFSM States */
enum link_state {
    LINK_DOWN,
    LINK_STARTING,
    LINK_UP,
    LINK_STOPPING,
    LINK_STATE_MAX
};

/* LFSM Actions */
enum lfsm_action_type {
    LFSM_ACT_LINK_UP,
    LFSM_ACT_LINK_DOWN,
    LFSM_ACT_MAX
};

int lfsm_register_link_state_notifier(struct notifier_block *nb);
int lfsm_unregister_link_state_notifier(struct notifier_block *nb);
int lfsm_link_up(void);
int lfsm_link_down(void);
enum link_state lfsm_get_link_state(void);
void lfsm_force_down(void);

#endif // LSFM_H