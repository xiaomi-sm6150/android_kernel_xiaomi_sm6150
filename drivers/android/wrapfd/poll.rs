// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2026 Google LLC.

use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use crate::wrap::WRAP_FOPS;

use kernel::{
    bindings,
    fs::LocalFile,
    new_mutex, new_poll_condvar,
    prelude::*,
    sync::{
        poll::{PollCondVar, PollTable},
        Arc, Mutex,
    },
    uapi,
};

#[derive(Default)]
struct EventInfo {
    fd: u32,
    monitored: u32,
    triggered: u32,
    timestamp: u64,
    seq_num: u32,
}

impl EventInfo {
    fn new(seq_num: u32, fd: u32) -> Self {
        Self {
            fd,
            monitored: 0,
            triggered: 0,
            timestamp: 0,
            seq_num,
        }
    }
}

type EventVector = KVec<Option<EventInfo>>;

#[pin_data]
pub(crate) struct FileMonitor {
    #[pin]
    condvar: PollCondVar,
    events_pending: AtomicBool,

    #[pin]
    events: Mutex<EventVector>,

    next_seq_num: AtomicU32,
}

#[derive(Clone, Copy)]
pub(crate) enum ProcessMode {
    Add,
    Delete,
}

struct SlotInfo {
    slot: usize,
    seq_num: u32,
}

impl FileMonitor {
    pub(crate) fn new() -> impl PinInit<Self> {
        pin_init!(Self {
            condvar <- new_poll_condvar!("FileMonitor::condvar"),
            events_pending: false.into(),
            events <- new_mutex!(EventVector::default()),
            next_seq_num: 0.into(),
        })
    }

    fn allocate_slot(&self, events: &mut EventVector, fd: u32) -> Result<usize> {
        let seq_num = self.next_seq_num.fetch_add(1, Ordering::Relaxed);

        let info = EventInfo::new(seq_num, fd);

        if let Some(slot) = events.iter().position(|entry| entry.is_none()) {
            events[slot] = Some(info);
            return Ok(slot);
        }

        let slot = events.len();

        // If this fails we'll have already incremented `self.next_seq_num` but that's not
        // problematic.
        events.push(Some(info), GFP_KERNEL)?;

        Ok(slot)
    }

    pub(crate) fn free_slot(&self, slot: usize) -> Result<()> {
        let mut guard = self.events.lock();
        if slot >= guard.len() || guard[slot].is_none() {
            return Err(ERANGE);
        }

        guard[slot] = None;

        Ok(())
    }

    pub(crate) fn generate_wrap_event(&self, slot: usize, event: u32) -> Result<()> {
        let should_wake = {
            let mut guard = self.events.lock();
            if slot >= guard.len() {
                return Err(ERANGE);
            }

            let Some(info) = &mut guard[slot] else {
                return Err(EINVAL);
            };

            if (info.monitored & event) == 0 {
                return Ok(());
            }

            info.triggered |= event;

            // SAFETY: Calling `ktime_get_ns` is always safe.
            info.timestamp = unsafe { bindings::ktime_get_ns() };

            !self.events_pending.swap(true, Ordering::Release)
        };

        if should_wake {
            self.condvar.notify_all();
        }

        Ok(())
    }

    pub(crate) fn process_monitor(
        filemon: &Arc<Self>,
        args: &uapi::wrapfd_monitor,
        mode: ProcessMode,
    ) -> Result<()> {
        if (args.events & !uapi::WRAPFD_EVENTS_MASK) != 0
            || (args.events & uapi::WRAPFD_EVENTS_MASK) == 0
        {
            return Err(EINVAL);
        }

        let Ok(local_file) = LocalFile::fget(args.fd) else {
            return Err(EBADF);
        };

        let Some(wrap) = WRAP_FOPS.borrow_private(&*local_file) else {
            return Err(EBADF);
        };

        let mut wrap_guard = wrap.inner.lock();
        if let Some(slot) = wrap_guard.get_slot(filemon) {
            let mut filemon_guard = filemon.events.lock();
            let Some(info) = &mut filemon_guard[slot] else {
                // This really should never happen.
                return Err(EINVAL);
            };

            match mode {
                ProcessMode::Add => {
                    if info.fd != args.fd {
                        /*
                         * Do not allow monitoring the same wrapfd file
                         * under different (dup'ed) FDs since that
                         * would cause one wrapfd operation to generate
                         * multiple events.
                         */
                        return Err(EBUSY);
                    }

                    filemon_guard[slot].as_mut().unwrap().monitored |= args.events;
                }
                ProcessMode::Delete => {
                    info.monitored &= !args.events;
                    info.triggered &= info.monitored;
                    if info.monitored == 0 {
                        _ = wrap_guard.detach_monitor(&filemon);
                        filemon_guard[slot] = None;
                    }
                    return Ok(());
                }
            }
        } else if matches!(mode, ProcessMode::Delete) {
            return Err(EINVAL);
        } else {
            let mut filemon_guard = filemon.events.lock();
            let slot = filemon.allocate_slot(&mut filemon_guard, args.fd)?;
            wrap_guard
                .attach_monitor(Arc::clone(filemon), slot)
                .inspect_err(|_err| {
                    filemon_guard[slot] = None;
                })?;
            filemon_guard[slot].as_mut().unwrap().monitored |= args.events;
        }

        Ok(())
    }

    pub(crate) fn query_events<F>(&self, max_num_events: usize, f: F) -> Result<()>
    where
        F: FnOnce(&[uapi::wrapfd_event]) -> Result,
    {
        let mut events = KVec::<uapi::wrapfd_event>::with_capacity(max_num_events, GFP_KERNEL)?;
        let mut slot_infos = KVec::<SlotInfo>::with_capacity(max_num_events, GFP_KERNEL)?;

        {
            let mut filemon_guard = self.events.lock();

            let mut has_more = false;
            for (slot, opt_info) in filemon_guard.iter_mut().enumerate() {
                let Some(info) = opt_info else {
                    continue;
                };

                if info.triggered == 0 {
                    continue;
                }

                let Ok(()) = slot_infos.push_within_capacity(SlotInfo {
                    slot,
                    seq_num: info.seq_num,
                }) else {
                    has_more = true;
                    break;
                };

                // If our previous `push_within_capacity` succeeded, so will this one, both vectors
                // have the same capacity.
                _ = events.push_within_capacity(uapi::wrapfd_event {
                    fd: info.fd,
                    events: info.triggered,
                    timestamp: info.timestamp,
                });

                info.triggered = 0;
            }

            if !has_more {
                self.events_pending.store(false, Ordering::Release);
            }
        }

        if let Err(err) = f(&events) {
            if events.is_empty() {
                // There's nothing for us to restore, so no need for wakeup either.
                return Err(err);
            }

            let should_wake = {
                let mut filemon_guard = self.events.lock();

                let mut restored_any = false;
                for (SlotInfo { slot, seq_num }, ev) in slot_infos.into_iter().zip(events.iter()) {
                    let Some(Some(info)) = filemon_guard.get_mut(slot) else {
                        continue;
                    };

                    if info.seq_num != seq_num {
                        // This slot has since been reused, there's nothing for us to restore here.
                        continue;
                    }

                    // Make sure that we only restore bits that are still monitored now, after all
                    // we briefly released our lock, so a `process_monitor` with
                    // `ProcessMode::Delete` may have been issued.
                    let restore_mask = ev.events & info.monitored;

                    if restore_mask == 0 {
                        continue;
                    }

                    // It wouldn't do to just assign `ev.events` back to `info.triggered`, since we
                    // released the lock albeit briefly, other events may have been or'ed into the
                    // `triggered` mask already.
                    info.triggered |= restore_mask;
                    restored_any = true;
                }

                restored_any && !self.events_pending.swap(true, Ordering::Release)
            };

            if should_wake {
                self.condvar.notify_all();
            }

            return Err(err);
        }

        Ok(())
    }

    pub(crate) fn poll(
        &self,
        file: &kernel::fs::File,
        table: PollTable<'_>,
    ) -> Result<bindings::__poll_t> {
        table.register_wait(file, &self.condvar);

        let mut mask = 0;
        if self.events_pending.load(Ordering::Acquire) {
            mask |= bindings::POLLIN | bindings::POLLRDNORM;
        }

        Ok(mask)
    }
}

impl PartialEq for FileMonitor {
    fn eq(&self, other: &Self) -> bool {
        core::ptr::eq(self, other)
    }
}

impl Eq for FileMonitor {}
