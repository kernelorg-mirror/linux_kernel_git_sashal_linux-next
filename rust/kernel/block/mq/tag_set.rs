// SPDX-License-Identifier: GPL-2.0

//! This module provides the `TagSet` struct to wrap the C `struct blk_mq_tag_set`.
//!
//! C header: [`include/linux/blk-mq.h`](srctree/include/linux/blk-mq.h)

use crate::{
    bindings,
    block::mq::{
        operations::OperationsVTable,
        request::RequestDataWrapper,
        Operations, //
    },
    error::to_result,
    prelude::*,
    types::Opaque, //
};
use core::{
    convert::TryInto,
    marker::PhantomData, //
};

/// A wrapper for the C `struct blk_mq_tag_set`.
///
/// `struct blk_mq_tag_set` contains a `struct list_head` and so must be pinned.
///
/// # Invariants
///
/// - `inner` is initialized and valid.
#[pin_data(PinnedDrop)]
#[repr(transparent)]
pub struct TagSet<T: Operations> {
    #[pin]
    inner: Opaque<bindings::blk_mq_tag_set>,
    _p: PhantomData<T>,
}

impl<T: Operations> TagSet<T> {
    /// Try to create a new tag set
    pub fn new(nr_hw_queues: u32, num_tags: u32, num_maps: u32) -> impl PinInit<Self, Error> {
        try_pin_init!(TagSet {
            inner <- Opaque::try_ffi_init(move |tag_set: *mut bindings::blk_mq_tag_set| {
                let cmd_size = core::mem::size_of::<RequestDataWrapper>().try_into()?;

                // SAFETY: `try_ffi_init` provides writable storage, and `Opaque` permits
                // uninitialized fields until the C initializer initializes them.
                unsafe {
                    tag_set.write_bytes(0, 1);
                    (*tag_set).ops = OperationsVTable::<T>::build();
                    (*tag_set).nr_hw_queues = nr_hw_queues;
                    (*tag_set).timeout = 0; // 0 means default which is 30Hz in C
                    (*tag_set).numa_node = bindings::NUMA_NO_NODE;
                    (*tag_set).queue_depth = num_tags;
                    (*tag_set).cmd_size = cmd_size;
                    (*tag_set).flags = 0;
                    (*tag_set).driver_data = core::ptr::null_mut::<crate::ffi::c_void>();
                    (*tag_set).nr_maps = num_maps;
                }

                // SAFETY: All input fields are initialized, and the C initializer initializes
                // the remaining fields, including the locks, in their pinned location.
                to_result(unsafe { bindings::blk_mq_alloc_tag_set(tag_set) })
            }),
            _p: PhantomData,
        })
    }

    /// Return the pointer to the wrapped `struct blk_mq_tag_set`
    pub(crate) fn raw_tag_set(&self) -> *mut bindings::blk_mq_tag_set {
        self.inner.get()
    }
}

#[pinned_drop]
impl<T: Operations> PinnedDrop for TagSet<T> {
    fn drop(self: Pin<&mut Self>) {
        // SAFETY: By type invariant `inner` is valid and has been properly
        // initialized during construction.
        unsafe { bindings::blk_mq_free_tag_set(self.inner.get()) };
    }
}

// SAFETY: It is safe to transfer ownership of `TagSet` between threads.
unsafe impl<T: Operations> Send for TagSet<T> {}

// SAFETY: It is safe to share `&TagSet` across threads. `TagSet` exposes no
// `&self` methods that mutate the contained `blk_mq_tag_set`.
unsafe impl<T: Operations> Sync for TagSet<T> {}
