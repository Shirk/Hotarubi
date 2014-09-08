/*******************************************************************************

    Copyright (C) 2014  René 'Shirk' Köcher
 
    This file is part of Hotarubi.

    Hotarubi is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
 
    Hotarubi is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
 
    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*******************************************************************************/

/* SLAB based cache allocator */

#include <hotarubi/lock.h>
#include <hotarubi/log/log.h>

#include <hotarubi/memory.h>
#include <hotarubi/vmconst.h>

#include <string.h>
#include <list.h>

/* NOTE: since these caches need to be SMP safe all methods below
 *       starting with an underscore (_) expect mem_cache.lock to be held
 *       while beeing called and will by themself do no further locking.
 */

namespace memory
{
namespace cache
{

struct mem_cache
{
	const char *name;
	bool compact;

	size_t obj_size;
	size_t obj_align;

	cache_obj_setup obj_setup = nullptr;
	cache_obj_erase obj_erase = nullptr;

	backend_alloc slab_alloc = nullptr;
	backend_free  slab_free  = nullptr;

	LIST_HEAD( free );
	LIST_HEAD( used );
	LIST_HEAD( full );

	// FIXME: HASH_TABLE( obj_map );
	LIST_HEAD( obj_map );

	mem_cache_stats_t stats;
	SpinLock lock;
};

struct slab
{
	unsigned refs;

	void *backing;
	LIST_LINK( slabs );
	SLIST_HEAD( bufctls );
};

struct bufctl_inline
{
	union
	{
		struct slab *slab;
		SLIST_LINK( link );
	};
};

struct bufctl
{
	union
	{
		struct slab *slab;
		SLIST_LINK( link );
	};

	LIST_LINK( bufctls );
	void *data;
};

/* ensure we can cast a bufctl to a bufctl_inline */
static_assert( offsetof( struct bufctl, slab ) == offsetof( struct bufctl_inline, slab ) &&
               offsetof( struct bufctl, link ) == offsetof( struct bufctl_inline, link ),
               "bufctl is not transparently castable to bufctl_inline !");

/* lockal cache pools:
 * cache_cache  - has to be a static allocation since it serves as supply for all
 *                other mem_caches.
 *
 * slab_cache   - used to allocate SLABS for caches where they can't be inlined
 *
 * bufctl_cache - same as slab_cache but for bufctl structs
 */
static struct mem_cache cache_cache;
static mem_cache_t slab_cache   = nullptr;
static mem_cache_t bufctl_cache = nullptr;

/* default backing storage allocators */
#ifdef KERNEL

static void*
_backing_page_alloc( size_t n )
{
	if( n % PAGE_SIZE != 0 )
	{
		/* round up to the nearest multiple of PAGE_SIZE */
		n += PAGE_SIZE - ( n % PAGE_SIZE );
	}
	return physmm::alloc_page_range( n / PAGE_SIZE );
}

static void
_backing_page_free( void *ptr, size_t n )
{
	if( n % PAGE_SIZE != 0 )
	{
		n += PAGE_SIZE - ( n % PAGE_SIZE );
	}
	physmm::free_page_range( ptr, n / PAGE_SIZE );
}

#else

extern void* _backing_page_alloc( size_t n );
extern void  _backing_page_free( void *ptr, size_t n );

#endif

/* fetch the SLAB from the end of the supplied buffer */
#define SLAB_INLINE( buffer ) \
	( ( struct slab* )( ( uintptr_t )( buffer ) + SLAB_SIZE - sizeof( struct slab ) ) )

/* fetch a SLAB from the slab-cache */
#define SLAB_EXTERN( cache ) \
	( ( struct slab* )get_object( cache ) )

/* fetch the bufctl from the end of the supplied buffer */
#define BUFCTL_INLINE( buffer, obj_size ) \
	( ( struct bufctl* )( ( uintptr_t )( buffer )  + obj_size ) )

/* fetch a bufctl from the bufctl-cache */
#define BUFCTL_EXTERN( cache ) \
	( ( struct bufctl* )get_object( cache ) )

#define BUFFER_MAGIC_SIZE sizeof( uint16_t )
#define BUFFER_MAGIC_WORD 0xaa55

static struct slab*
_slab_alloc( struct mem_cache *cache )
{
	void *backing = cache->slab_alloc( SLAB_SIZE );

	cache->stats.allocation += SLAB_SIZE;

	struct slab *slab;
	size_t buff_size, buff_avail;

	if( backing == nullptr )
	{
		goto err_no_backing;
	}

	slab = ( ( cache->compact ) ? ( SLAB_INLINE( backing ) )
	                            : ( SLAB_EXTERN( slab_cache ) ) );

	if( slab == nullptr )
	{
		goto err_no_slab;
	}

	slab->refs = 0;
	slab->backing = backing;
	INIT_LIST( slab->slabs );
	INIT_SLIST( slab->bufctls );

	/* calculate the size of a single buffer including alignment and possible
	 * inline structures
	 */
	buff_size = cache->obj_size + BUFFER_MAGIC_SIZE + ( ( cache->compact ) ? sizeof( struct bufctl_inline ) : 0 );
	while( buff_size % cache->obj_align != 0 )
	{
		++buff_size;
	}

	/* calculate the number of buffers in the slab */
	buff_avail = ( ( cache->compact == false ) ? SLAB_SIZE : SLAB_SIZE - sizeof( struct slab ) ) / buff_size;

	if( buff_avail == 0 || buff_size > SLAB_SIZE )
	{
		goto err_no_bufctl;
	}

	for( size_t i = 0; i < buff_avail; ++i )
	{
		/* fill the slab */
		void *buffer = ( void* )( ( uintptr_t )backing + buff_size * i );
		uint16_t *magic = ( uint16_t* )( ( uintptr_t )buffer + cache->obj_size );

		struct bufctl *bufctl = ( ( cache->compact ) ? ( BUFCTL_INLINE( buffer, cache->obj_size + BUFFER_MAGIC_SIZE ) )
		                                             : ( BUFCTL_EXTERN( bufctl_cache ) ) );

		if( bufctl == nullptr )
		{
			goto err_no_bufctl;
		}

		*magic = BUFFER_MAGIC_WORD;

		if( cache->obj_setup )
		{
			cache->obj_setup( buffer, cache->obj_size );
		}

		if( !cache->compact )
		{
			/* external buffers are linked to be able to look up the buffer
			 * with only the address */
			bufctl->data = buffer;
			list_add( &cache->obj_map, &bufctl->bufctls );
		}
		slist_add( &slab->bufctls, &bufctl->link );

		++cache->stats.buffers;
	}

	list_add( &cache->free, &slab->slabs );
	++cache->stats.slabs;
	return slab;

/* all the things that may go wrong.. */
err_no_bufctl:
	SLIST_FOREACH_MUTABLE( item, &slab->bufctls )
	{
		struct bufctl *bufctl = SLIST_ENTRY( item, struct bufctl, link );

		slist_del( &slab->bufctls, &bufctl->link );
		list_del( &bufctl->bufctls );

		put_object( bufctl_cache, bufctl );
		--cache->stats.buffers;
	}
	put_object( slab_cache, slab );

err_no_slab:
	cache->slab_free( backing, SLAB_SIZE );
	cache->stats.allocation -= SLAB_SIZE;

err_no_backing:
	return nullptr;
}

static void
_slab_release( struct mem_cache *cache, struct slab *slab )
{
	void *backing = slab->backing;

	if( backing != nullptr )
	{
		if( !cache->compact )
		{
			/* huge caches need their parts to be returned to the space caches */
			SLIST_FOREACH_MUTABLE( item, &slab->bufctls )
			{
				struct bufctl *bufctl = SLIST_ENTRY( item, struct bufctl, link );

				if( cache->obj_erase )
				{
					void *object;

					if( cache->compact )
					{
						object = ( void* )( ( uintptr_t )bufctl - cache->obj_size - BUFFER_MAGIC_SIZE );
					}
					else
					{
						object = bufctl->data;
					}
					cache->obj_erase( object );
				}

				slist_del( &slab->bufctls, &bufctl->link );
				list_del( &bufctl->bufctls );

				put_object( bufctl_cache, bufctl );
				--cache->stats.buffers;
			}
			put_object( slab_cache, slab );
			--cache->stats.slabs;
		}
		/* the rest is as simple as releasing the backing storage */
		cache->slab_free( backing, SLAB_SIZE );
		cache->stats.allocation -= SLAB_SIZE;
	}
}

static void*
_slab_get_object( struct mem_cache *cache, struct slab *slab )
{
	if( ! slist_empty( &slab->bufctls ) )
	{
		struct bufctl *bufctl = SLIST_HEAD_ENTRY( &slab->bufctls, struct bufctl, link );

		slist_del( &slab->bufctls, &bufctl->link );
		bufctl->slab = slab;
		++slab->refs;
		if( cache->compact )
		{
			return ( void* )( ( uintptr_t )bufctl - ( cache->obj_size + BUFFER_MAGIC_SIZE ) );
		}
		else
		{
			return bufctl->data;
		}
	}

	return nullptr;
}

static void
_slab_put_object( struct slab *slab, struct bufctl *bufctl )
{
	if( slab->refs )
	{
		slist_add( &slab->bufctls, &bufctl->link );
		slab->refs--;
	}
}

static inline bool
_slab_full( struct slab *slab )
{
	return slist_empty( &slab->bufctls );
}

static inline bool
_slab_empty( struct slab *slab )
{
	return slab->refs == 0;
}

static void
_cache_sort( struct mem_cache *cache )
{
	/* migrate partial slabs from the full list */
	LIST_FOREACH_MUTABLE( ptr, &cache->full )
	{
		struct slab *slab = LIST_ENTRY( ptr, struct slab, slabs );
		if( !_slab_full( slab ) )
		{
			list_del( ptr );
			list_add_tail( &cache->used, ptr );
		}
	}
	/* migrate partial slabs from the free list */
	LIST_FOREACH_MUTABLE( ptr, &cache->free )
	{
		struct slab *slab = LIST_ENTRY( ptr, struct slab, slabs );
		if( !_slab_empty( slab ) )
		{
			list_del( ptr );
			list_add_tail( &cache->used, ptr );
		}
	}

	/* migrate empty or full slabs from the used list */
	LIST_FOREACH_MUTABLE( ptr, &cache->used )
	{
		struct slab *slab = LIST_ENTRY( ptr, struct slab, slabs );
		if( _slab_full( slab ) )
		{
			list_del( ptr );
			list_add_tail( &cache->full, ptr );
		}
		else if( _slab_empty( slab ) )
		{
			list_del( ptr );
			list_add_tail( &cache->free, ptr );
		}
	}

}

static void
_cache_init( struct mem_cache *cache, const char *name, size_t size, size_t align,
             cache_obj_setup setup = nullptr, cache_obj_erase erase = nullptr,
             backend_alloc back_alloc = nullptr, backend_free back_free = nullptr )
{
	cache->name       = name;
	cache->obj_size   = size;
	cache->obj_align  = ( align ) ? align : 1;
	cache->obj_setup  = setup;
	cache->obj_erase  = erase;
	cache->slab_alloc = ( back_alloc ) ? back_alloc : _backing_page_alloc;
	cache->slab_free  = ( back_free )  ? back_free  : _backing_page_free;
	cache->compact    = cache->obj_size < SLAB_MAX_FRAGMENT_SIZE;
	
	memset( &cache->stats, 0, sizeof( mem_cache_stats_t ) );

	INIT_LIST( cache->free );
	INIT_LIST( cache->used );
	INIT_LIST( cache->full );
	INIT_LIST( cache->obj_map );
}

void*
get_object( mem_cache_t cache )
{
	void *object;
	LIST_HEAD( *slab_list );

	cache->lock.lock();

	if( list_empty( &cache->used ) )
	{
		if( list_empty( &cache->free ) )
		{
			/* need more slabs.. */
			if( _slab_alloc( cache ) == nullptr )
			{
				return nullptr;
			}
			++cache->stats.cache_misses;
			--cache->stats.cache_hits; /* compensate for the ++ below */
		}
		slab_list = &cache->free;
	}
	else
	{
		slab_list = &cache->used;
	}

	object = _slab_get_object( cache, LIST_HEAD_ENTRY( slab_list, struct slab, slabs ) );
	if( object )
	{
		++cache->stats.cache_hits;
		_cache_sort( cache );
	}

	cache->lock.unlock();
	return object;
}

void
put_object( mem_cache_t cache, void *ptr )
{
	struct bufctl *bufctl = nullptr;
	uint16_t *magic = nullptr;

	cache->lock.lock();

	if( cache->compact )
	{
		/* no need for hash lookups, we can access the bufctl from ptr */
		bufctl = BUFCTL_INLINE( ptr, cache->obj_size + 2 );
	}
	else
	{
		/* FIXME: hash lookup would be nice */
		LIST_FOREACH( item, &cache->obj_map )
		{
			bufctl = ( struct bufctl* )LIST_ENTRY( item, struct bufctl, bufctls );
			if( bufctl->data == ptr )
			{
				break;
			}
			bufctl = nullptr;
		}
	}

	if( bufctl != nullptr )
	{
		magic = ( uint16_t* )( ( uintptr_t )ptr + cache->obj_size );
		if( *magic != BUFFER_MAGIC_WORD )
		{
			*magic = BUFFER_MAGIC_WORD;
		}
		_slab_put_object( bufctl->slab, bufctl );
		_cache_sort( cache );
	}

	cache->lock.unlock();
}

#undef BUFCTL_INLINE
#undef BUFCTL_EXTERN

#undef SLAB_INLINE
#undef SLAB_EXTERN

mem_cache_t
create( const char *name, size_t size, size_t align,
        cache_obj_setup setup, cache_obj_erase erase,
        backend_alloc back_alloc, backend_free back_free )
{
	mem_cache_t cache = ( mem_cache_t )get_object( &cache_cache );

	if( cache != nullptr )
	{
		if( back_alloc == nullptr || back_free == nullptr )
		{
			back_alloc = _backing_page_alloc;
			back_free  = _backing_page_free;
		}

		_cache_init( cache, name, size, align, setup, erase, back_alloc, back_free );
	}
	return cache;
}

bool
reap( mem_cache_t cache )
{
	cache->lock.lock();
	/* try to reclaim space by releasing free slabs */
	if( list_empty( &cache->free ) )
	{
		cache->lock.unlock();
		return false;
	}

	LIST_FOREACH_MUTABLE( item, &cache->free )
	{
		_slab_release( cache, LIST_ENTRY( item, struct slab, slabs ) );
	}
	cache->lock.unlock();
	return true;
}

bool
release( mem_cache_t cache, bool force )
{
	cache->lock.lock();

	if( ( !list_empty( &cache->full )   ||
		  !list_empty( &cache->used ) ) &&
		  force == false )
	{
		log::printk( "cache::release: refusing to free '%s' which still contains allocations!\n",
		             cache->name );

		cache->lock.unlock();
		return false;
	}

	if( force )
	{
		LIST_FOREACH_MUTABLE( item, &cache->full )
		{
			log::printk( "releasing full slab: %p\n", LIST_ENTRY( item, struct slab, slabs ) );
			_slab_release( cache, LIST_ENTRY( item, struct slab, slabs ) );
		}

		LIST_FOREACH_MUTABLE( item, &cache->used )
		{
			log::printk( "releasing partial slab: %p\n", LIST_ENTRY( item, struct slab, slabs ) );
			_slab_release( cache, LIST_ENTRY( item, struct slab, slabs ) );
		}
	}
	LIST_FOREACH_MUTABLE( item, &cache->free )
	{
		log::printk( "releasing free slab: %p\n", LIST_ENTRY( item, struct slab, slabs ) );
		_slab_release( cache, LIST_ENTRY( item, struct slab, slabs ) );
	}

	cache->lock.unlock();
	put_object( &cache_cache, cache );

	return true;
}

void
stats( mem_cache_t cache, mem_cache_stats_t &statbuf )
{
	cache->lock.lock();
	memcpy( &statbuf, &cache->stats, sizeof( mem_cache_stats_t ) );
	cache->lock.unlock();
}

void
init( void )
{
	log::printk( "Initializing SLAB object cache..\n" );
	_cache_init( &cache_cache, "cache::cache-pool", sizeof( struct mem_cache ), 4 );

	slab_cache   = create( "cache::slab-pool", sizeof( struct slab ), 4 );
	bufctl_cache = create( "cache::bufctl-pool", sizeof( struct bufctl ), 4 );
}

};
};
