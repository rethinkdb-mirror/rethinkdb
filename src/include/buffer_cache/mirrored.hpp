
#ifndef __MIRRORED_CACHE_HPP__
#define __MIRRORED_CACHE_HPP__

#include "event_queue.hpp"
#include "cpu_context.hpp"

// This cache doesn't actually do any operations itself. Instead, it
// provides a framework that collects all components of the cache
// (memory allocation, page lookup, page replacement, writeback, etc.)
// into a coherent whole. This allows easily experimenting with
// various components of the cache to improve performance.

template <class config_t>
struct aio_context : public alloc_mixin_t<tls_small_obj_alloc_accessor<typename config_t::alloc_t>, aio_context<config_t> > {
    typedef typename config_t::serializer_t serializer_t;
    typedef typename serializer_t::block_id_t block_id_t;

    void *user_state;
    block_id_t block_id;
#ifndef NDEBUG
    // We use this member in debug mode to ensure all operations
    // associated with the context occur on the same event queue.
    event_queue_t *event_queue;
#endif
};

template <class config_t>
struct mirrored_cache_t : public config_t::serializer_t,
                          public config_t::buffer_alloc_t,
                          public config_t::page_map_t,
                          public config_t::page_repl_t,
                          public config_t::writeback_t
{
public:
    typedef typename config_t::serializer_t serializer_t;
    typedef typename serializer_t::block_id_t block_id_t;
    typedef typename config_t::page_repl_t page_repl_t;
    typedef typename config_t::writeback_t writeback_t;
    typedef typename config_t::buffer_alloc_t buffer_alloc_t;
    typedef typename config_t::page_map_t page_map_t;
    typedef typename config_t::conn_fsm_t conn_fsm_t;
    typedef aio_context<config_t> aio_context_t;

    // For now the transaction object contains nothing other than the
    // event_queue pointer, so we don't create an extra structure.
    typedef event_queue_t transaction_t;

public:
    // TODO: how do we design communication between cache policies?
    // Should they all have access to the cache, or should they only
    // be given access to each other as necessary? The first is more
    // flexible as anyone can access anyone else, but encourages too
    // many dependencies. The second is more strict, but might not be
    // extensible when some policy implementation requires access to
    // components it wasn't originally given.
    mirrored_cache_t(size_t _block_size, size_t _max_size) : 
        serializer_t(_block_size),
        page_repl_t(_block_size, _max_size, this, this),
        writeback_t(this)
        {}

    void start() {
        writeback_t::start();
    }

    // Transaction API
    transaction_t* begin_transaction() {
        event_queue_t *event_queue = get_cpu_context()->event_queue;
        return event_queue;
    }
    void end_transaction(transaction_t* transaction) {
        assert(transaction == get_cpu_context()->event_queue);
    }

    // TODO: each operation can only be performed within a
    // transaction. Much the API nicer (from the OOP/C++ point of
    // view), and move the following methods into a separate
    // transaction class.
    void* allocate(transaction_t* tm, block_id_t *block_id) {
        assert(tm == get_cpu_context()->event_queue);
        
        *block_id = serializer_t::gen_block_id();
        void *block = buffer_alloc_t::malloc(serializer_t::block_size);
        page_map_t::set(*block_id, block);
        page_repl_t::pin(*block_id);
        
        return block;
    }
    
    void* acquire(transaction_t* tm, block_id_t block_id, void *state) {
        assert(tm == get_cpu_context()->event_queue);
        
        // TODO: we might get a request for a block id while the block
        // with that block id is still loading (consider two requests
        // in a row). We need to keep track of this so we don't
        // unnecessarily double IO and/or lose memory.

        void *block = page_map_t::find(block_id);
        if(!block) {
            void *buf = buffer_alloc_t::malloc(serializer_t::block_size);
            aio_context_t *ctx = new aio_context_t();
            ctx->user_state = state;
            ctx->block_id = block_id;
#ifndef NDEBUG            
            ctx->event_queue = tm;
#endif

            do_read(tm, block_id, buf, ctx);
        } else {
            page_repl_t::pin(block_id);
        }

        return block;
    }

    block_id_t release(transaction_t* tm, block_id_t block_id, void *block, bool dirty, void *state) {
        assert(tm == get_cpu_context()->event_queue);
        
        block_id_t new_block_id = block_id;
        if(dirty) {
            new_block_id = writeback_t::mark_dirty(tm, block_id, block, state);
            // Already pinned by 'acquire'. Will unpin in aio_complete
            // when the block is written
        } else {
            page_repl_t::unpin(block_id);
        }

        return new_block_id;
    }

    void aio_complete(aio_context_t *ctx, void *block, bool written) {
#ifndef NDEBUG            
        assert(ctx->event_queue = get_cpu_context()->event_queue);
#endif

        block_id_t block_id = ctx->block_id;
        delete ctx;
        if(written) {
            page_repl_t::unpin(block_id);
        } else {
            page_map_t::set(block_id, block);
            page_repl_t::pin(block_id);
        }
    }
};

#endif // __MIRRORED_CACHE_HPP__

