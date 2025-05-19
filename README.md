# LFSM: Link Finite State Machine Kernel Module

## Overview

LFSM (Link Finite State Machine) is a Linux kernel module that manages the state of a logical link using a finite state machine. It provides a generic, thread-safe interface for transitioning the link between UP and DOWN states, with support for asynchronous actions, timeouts, sysfs introspection, and user-space notifications via Generic Netlink.

## Building

To build the LFSM kernel module, ensure you have the Linux kernel and build tools installed. Then, run the following command from the root of your kernel source tree:


```sh
make -j$(nproc) M=drivers/lfsm modules
```

## Install

You can further install the module with the following commands:

```sh
make -j$(nproc) M=drivers/lfsm modules_install
depmod
modprobe lfsm
```

## Features

- Finite State Machine for link state transitions (`LINK_DOWN`, `LINK_STARTING`, `LINK_UP`, `LINK_STOPPING`)
- Asynchronous action queue using kfifo and workqueues
- Timeout handling for transitions
- Notifier chain for kernel clients to subscribe to link state changes
- Generic Netlink interface for user-space notifications and control
- Sysfs attributes for state and queue inspection

## API Reference

See exported functions in `lfsm.c` for details.

## Sysfs Interface

- `/sys/kernel/lfsm/state` — Shows the current link state.
- `/sys/kernel/lfsm/queue` — Shows the pending action queue.

## Netlink Interface

- **Family:** `lfsm_notify`
- **Multicast group:** `lfsm_events`
- **Commands:** `LINK_UP`, `LINK_DOWN`, `CANCEL`, `NOTIFY`
- **Attribute:** `LINK_STATE` (`u32`)

## Usage Example

```c
static int my_link_notifier(struct notifier_block *nb, unsigned long val, void *data) {
    // handle state change
    return NOTIFY_OK;
}
static struct notifier_block nb = {
    .notifier_call = my_link_notifier,
};
register_link_state_notifier(&nb);
lfsm_link_up();
```

## Authors

- Christopher Denny <christopherscottdenny@gmail.com>

## License

GPL-3.0