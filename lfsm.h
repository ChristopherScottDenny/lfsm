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

int register_link_state_notifier(struct notifier_block *nb);
int unregister_link_state_notifier(struct notifier_block *nb);
int link_up(void);
int link_down(void);
enum link_state get_link_state(void);
void cancel_lfsm_actions_and_force_down(void);

#endif // LSFM_H