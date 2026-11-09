// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2026 Google LLC.

use core::sync::atomic::{AtomicBool, Ordering};

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
}

impl EventInfo {
    fn new(fd: u32) -> Self {
        Self {
            fd,
            monitored: 0,
            triggered: 0,
            timestamp: 0,
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
}

#[derive(Clone, Copy)]
pub(crate) enum ProcessMode {
    Add,
    Delete,
}

impl FileMonitor {
    pub(crate) fn new() -> impl PinInit<Self> {
        pin_init!(Self {
            condvar <- new_poll_condvar!("FileMonitor::condvar"),
            events_pending: false.into(),
            events <- new_mutex!(EventVector::default()),
        })
    }

    fn allocate_slot(events: &mut EventVector, fd: u32) -> Result<usize> {
        if let Some(slot) = events.iter().position(|entry| entry.is_none()) {
            events[slot] = Some(EventInfo::new(fd));
            return Ok(slot);
        }

        let slot = events.len();
        events.push(Some(EventInfo::new(fd)), GFP_KERNEL)?;

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

        // SAFETY: There are no `fdget_pos` calls on the current thread.
        let file = unsafe { LocalFile::assume_no_fdget_pos(local_file) };

        let Some(wrap) = WRAP_FOPS.borrow_private(&*file) else {
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
            let slot = Self::allocate_slot(&mut filemon_guard, args.fd)?;
            wrap_guard
                .attach_monitor(Arc::clone(filemon), slot)
                .inspect_err(|_err| {
                    filemon_guard[slot] = None;
                })?;
            filemon_guard[slot].as_mut().unwrap().monitored |= args.events;
        }

        Ok(())
    }

    pub(crate) fn query_events(&self, events: &mut KVec<uapi::wrapfd_event>) -> Result<()> {
        {
            let mut filemon_guard = self.events.lock();

            let mut has_more = false;
            for opt_info in &mut *filemon_guard {
                let Some(info) = opt_info else {
                    continue;
                };

                if info.triggered == 0 {
                    continue;
                }

                if events
                    .push_within_capacity(uapi::wrapfd_event {
                        fd: info.fd,
                        events: info.triggered,
                        timestamp: info.timestamp,
                    })
                    .is_err()
                {
                    has_more = true;
                    break;
                }

                info.triggered = 0;
            }

            if !has_more {
                self.events_pending.store(false, Ordering::Release);
            }
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
