/* lru_cache.c - Simple LRU Cache Implementation
 * - Thread-safe operations with reader-writer locks
 * - O(1) get, put, and delete operations
 * - Custom memory allocators support
 * - Statistics tracking
 *     - Hits
 *     - Misses
 *     - Evictions
 * - Eviction callbacks
 * - Iterator support for cache traversal
 * - Configurable capacity with dynamic resizing
 * - Key-value pair deep copying
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>

#define LRU_CACHE_DEFAULT_CAPACITY 1024
#define LRU_CACHE_MIN_CAPACITY 1
#define LRU_CACHE_LOAD_FACTOR 0.75

typedef enum
{
        LRU_SUCCESS = 0,
        LRU_ERROR_NOMEM = -1,
        LRU_ERROR_INVALID_ARG = -2,
        LRU_ERROR_NOT_FOUND = -3,
        LRU_ERROR_LOCK = -4,
        LRU_ERROR_FULL = -5
} lru_error_t;

typedef struct lru_cache lru_cache_t;
typedef struct lru_node lru_node_t;
typedef struct lru_hash_entry lru_hash_entry_t;

typedef void *(*lru_malloc_fn) (size_t size);
typedef void (*lru_free_fn) (void *ptr);
typedef void *(*lru_copy_fn) (const void *data, size_t size);
typedef void (*lru_destroy_fn) (void *data);
typedef unsigned long (*lru_hash_fn) (const void *key, size_t key_size);
typedef int (*lru_compare_fn) (const void *key1, size_t size1,
                               const void *key2, size_t size2);
typedef void (*lru_eviction_fn) (const void *key, size_t key_size,
                                 void *value, size_t value_size,
                                 void *user_data);

typedef struct lru_allocator
{
        lru_malloc_fn malloc_fn;
        lru_free_fn free_fn;
        lru_copy_fn copy_fn;
        lru_destroy_fn destroy_fn;
} lru_allocator_t;

typedef struct lru_stats
{
        uint64_t hits;
        uint64_t misses;
        uint64_t evictions;
        uint64_t insertions;
        uint64_t deletions;
        uint64_t collisions;
        size_t  current_size;
        size_t  peak_size;
} lru_stats_t;

struct lru_node
{
        void   *key;
        size_t  key_size;
        void   *value;
        size_t  value_size;
        lru_node_t *prev;
        lru_node_t *next;
        lru_hash_entry_t *hash_entry;
};

struct lru_hash_entry
{
        lru_node_t *node;
        lru_hash_entry_t *next;
};

struct lru_cache
{
        size_t  capacity;
        size_t  size;
        size_t  hash_table_size;

        lru_node_t *head;
        lru_node_t *tail;
        lru_hash_entry_t **hash_table;

        lru_allocator_t allocator;
        lru_hash_fn hash_fn;
        lru_compare_fn compare_fn;
        lru_eviction_fn eviction_fn;
        void   *eviction_user_data;

        lru_stats_t stats;
        pthread_rwlock_t lock;

        bool    thread_safe;
        bool    track_stats;
};

typedef struct lru_iterator
{
        lru_cache_t *cache;
        lru_node_t *current;
        bool    locked;
} lru_iterator_t;

static void *
default_malloc (size_t size)
{
        return malloc (size);
}

static void
default_free (void *ptr)
{
        free (ptr);
}

static void *
default_copy (const void *data, size_t size)
{
        void   *copy;

        copy = malloc (size);
        if (copy != NULL)
                memcpy (copy, data, size);

        return copy;
}

static void
default_destroy (void *data)
{
        free (data);
}

static unsigned long
default_hash (const void *key, size_t key_size)
{
        unsigned long hash;
        const unsigned char *ptr;
        size_t  i;

        hash = 5381;
        ptr = (const unsigned char *) key;

        for (i = 0; i < key_size; i++)
                hash = ((hash << 5) + hash) + ptr[i];

        return hash;
}

static int
default_compare (const void *key1, size_t size1,
                 const void *key2, size_t size2)
{
        if (size1 != size2)
                return (int) (size1 - size2);

        return memcmp (key1, key2, size1);
}

static size_t
calculate_hash_table_size (size_t capacity)
{
        size_t  size;

        size = (size_t) (capacity / LRU_CACHE_LOAD_FACTOR);

        size--;
        size |= size >> 1;
        size |= size >> 2;
        size |= size >> 4;
        size |= size >> 8;
        size |= size >> 16;
        size++;

        return size;
}

static lru_node_t *
create_node (lru_cache_t *cache, const void *key, size_t key_size,
             const void *value, size_t value_size)
{
        lru_node_t *node;

        node = cache->allocator.malloc_fn (sizeof (lru_node_t));
        if (node == NULL)
                return NULL;

        node->key = cache->allocator.copy_fn (key, key_size);
        if (node->key == NULL)
        {
                cache->allocator.free_fn (node);
                return NULL;
        }

        node->value = cache->allocator.copy_fn (value, value_size);
        if (node->value == NULL)
        {
                cache->allocator.destroy_fn (node->key);
                cache->allocator.free_fn (node);
                return NULL;
        }

        node->key_size = key_size;
        node->value_size = value_size;
        node->prev = NULL;
        node->next = NULL;
        node->hash_entry = NULL;

        return node;
}

static void
destroy_node (lru_cache_t *cache, lru_node_t *node)
{
        if (node == NULL)
                return;

        if (node->key != NULL)
                cache->allocator.destroy_fn (node->key);

        if (node->value != NULL)
                cache->allocator.destroy_fn (node->value);

        cache->allocator.free_fn (node);
}

static void
remove_from_list (lru_cache_t *cache, lru_node_t *node)
{
        if (node->prev != NULL)
                node->prev->next = node->next;
        else
                cache->head = node->next;

        if (node->next != NULL)
                node->next->prev = node->prev;
        else
                cache->tail = node->prev;
}

static void
add_to_front (lru_cache_t *cache, lru_node_t *node)
{
        node->next = cache->head;
        node->prev = NULL;

        if (cache->head != NULL)
                cache->head->prev = node;

        cache->head = node;

        if (cache->tail == NULL)
                cache->tail = node;
}

static void
move_to_front (lru_cache_t *cache, lru_node_t *node)
{
        if (cache->head == node)
                return;

        remove_from_list (cache, node);
        add_to_front (cache, node);
}

static unsigned long
hash_key (lru_cache_t *cache, const void *key, size_t key_size)
{
        return cache->hash_fn (key, key_size) % cache->hash_table_size;
}

static lru_node_t *
find_in_hash_table (lru_cache_t *cache, const void *key, size_t key_size,
                    lru_hash_entry_t ***prev_entry_ptr)
{
        unsigned long hash;
        lru_hash_entry_t **entry_ptr;
        lru_hash_entry_t *entry;

        hash = hash_key (cache, key, key_size);
        entry_ptr = &cache->hash_table[hash];

        while ((entry = *entry_ptr) != NULL)
        {
                if (cache->compare_fn
                    (entry->node->key, entry->node->key_size, key,
                     key_size) == 0)
                {
                        if (prev_entry_ptr != NULL)
                                *prev_entry_ptr = entry_ptr;
                        return entry->node;
                }

                entry_ptr = &entry->next;

                if (cache->track_stats)
                        cache->stats.collisions++;
        }

        if (prev_entry_ptr != NULL)
                *prev_entry_ptr = entry_ptr;

        return NULL;
}

static int
add_to_hash_table (lru_cache_t *cache, lru_node_t *node)
{
        unsigned long hash;
        lru_hash_entry_t *entry;

        entry = cache->allocator.malloc_fn (sizeof (lru_hash_entry_t));
        if (entry == NULL)
                return LRU_ERROR_NOMEM;

        hash = hash_key (cache, node->key, node->key_size);
        entry->node = node;
        entry->next = cache->hash_table[hash];
        cache->hash_table[hash] = entry;

        node->hash_entry = entry;

        return LRU_SUCCESS;
}

static void
remove_from_hash_table (lru_cache_t *cache, lru_node_t *node)
{
        lru_hash_entry_t **entry_ptr;
        lru_hash_entry_t *entry;

        find_in_hash_table (cache, node->key, node->key_size, &entry_ptr);

        if (entry_ptr != NULL && *entry_ptr != NULL)
        {
                entry = *entry_ptr;
                *entry_ptr = entry->next;
                cache->allocator.free_fn (entry);
        }
}

static int
evict_lru (lru_cache_t *cache)
{
        lru_node_t *lru_node;

        if (cache->tail == NULL)
                return LRU_ERROR_NOT_FOUND;

        lru_node = cache->tail;

        if (cache->eviction_fn != NULL)
        {
                cache->eviction_fn (lru_node->key, lru_node->key_size,
                                    lru_node->value, lru_node->value_size,
                                    cache->eviction_user_data);
        }

        remove_from_hash_table (cache, lru_node);
        remove_from_list (cache, lru_node);

        if (cache->track_stats)
                cache->stats.evictions++;

        cache->size--;
        destroy_node (cache, lru_node);

        return LRU_SUCCESS;
}

lru_cache_t *
lru_cache_create (size_t capacity)
{
        lru_cache_t *cache = calloc (1, sizeof (lru_cache_t));

        if (capacity < LRU_CACHE_MIN_CAPACITY)
                capacity = LRU_CACHE_DEFAULT_CAPACITY;

        cache->capacity = capacity;
        cache->hash_table_size = calculate_hash_table_size (capacity);

        cache->hash_table = calloc (cache->hash_table_size,
                                    sizeof (lru_hash_entry_t *));
        if (cache->hash_table == NULL)
        {
                free (cache);
                return NULL;
        }

        cache->allocator.malloc_fn = default_malloc;
        cache->allocator.free_fn = default_free;
        cache->allocator.copy_fn = default_copy;
        cache->allocator.destroy_fn = default_destroy;

        cache->hash_fn = default_hash;
        cache->compare_fn = default_compare;

        cache->thread_safe = true;
        cache->track_stats = true;

        if (pthread_rwlock_init (&cache->lock, NULL) != 0)
        {
                free (cache->hash_table);
                free (cache);
                return NULL;
        }

        return cache;
}

int
lru_cache_set_allocator (lru_cache_t *cache, const lru_allocator_t *allocator)
{
        if (cache == NULL || allocator == NULL)
                return LRU_ERROR_INVALID_ARG;

        if (cache->size > 0)
                return LRU_ERROR_INVALID_ARG;

        if (allocator->malloc_fn != NULL)
                cache->allocator.malloc_fn = allocator->malloc_fn;

        if (allocator->free_fn != NULL)
                cache->allocator.free_fn = allocator->free_fn;

        if (allocator->copy_fn != NULL)
                cache->allocator.copy_fn = allocator->copy_fn;

        if (allocator->destroy_fn != NULL)
                cache->allocator.destroy_fn = allocator->destroy_fn;

        return LRU_SUCCESS;
}

int
lru_cache_set_hash_function (lru_cache_t *cache, lru_hash_fn hash_fn)
{
        if (cache == NULL || hash_fn == NULL)
                return LRU_ERROR_INVALID_ARG;

        if (cache->size > 0)
                return LRU_ERROR_INVALID_ARG;

        cache->hash_fn = hash_fn;
        return LRU_SUCCESS;
}

int
lru_cache_set_compare_function (lru_cache_t *cache, lru_compare_fn compare_fn)
{
        if (cache == NULL || compare_fn == NULL)
                return LRU_ERROR_INVALID_ARG;

        if (cache->size > 0)
                return LRU_ERROR_INVALID_ARG;

        cache->compare_fn = compare_fn;
        return LRU_SUCCESS;
}

int
lru_cache_set_eviction_callback (lru_cache_t *cache,
                                 lru_eviction_fn eviction_fn, void *user_data)
{
        if (cache == NULL)
                return LRU_ERROR_INVALID_ARG;

        cache->eviction_fn = eviction_fn;
        cache->eviction_user_data = user_data;

        return LRU_SUCCESS;
}

int
lru_cache_put (lru_cache_t *cache, const void *key, size_t key_size,
               const void *value, size_t value_size)
{
        lru_node_t *node;
        int     result;

        if (cache == NULL || key == NULL || value == NULL ||
            key_size == 0 || value_size == 0)
                return LRU_ERROR_INVALID_ARG;

        if (cache->thread_safe && pthread_rwlock_wrlock (&cache->lock) != 0)
                return LRU_ERROR_LOCK;

        node = find_in_hash_table (cache, key, key_size, NULL);

        if (node != NULL)
        {
                void   *new_value;

                new_value = cache->allocator.copy_fn (value, value_size);
                if (new_value == NULL)
                {
                        result = LRU_ERROR_NOMEM;
                        goto cleanup;
                }

                cache->allocator.destroy_fn (node->value);
                node->value = new_value;
                node->value_size = value_size;

                move_to_front (cache, node);
                result = LRU_SUCCESS;
                goto cleanup;
        }

        node = create_node (cache, key, key_size, value, value_size);
        if (node == NULL)
        {
                result = LRU_ERROR_NOMEM;
                goto cleanup;
        }

        if (cache->size >= cache->capacity)
        {
                result = evict_lru (cache);
                if (result != LRU_SUCCESS)
                {
                        destroy_node (cache, node);
                        goto cleanup;
                }
        }

        result = add_to_hash_table (cache, node);
        if (result != LRU_SUCCESS)
        {
                destroy_node (cache, node);
                goto cleanup;
        }

        add_to_front (cache, node);
        cache->size++;

        if (cache->track_stats)
        {
                cache->stats.insertions++;
                cache->stats.current_size = cache->size;
                if (cache->size > cache->stats.peak_size)
                        cache->stats.peak_size = cache->size;
        }

        result = LRU_SUCCESS;

      cleanup:
        if (cache->thread_safe)
                pthread_rwlock_unlock (&cache->lock);

        return result;
}

int
lru_cache_get (lru_cache_t *cache, const void *key, size_t key_size,
               void **value, size_t *value_size)
{
        lru_node_t *node;
        int     result;

        if (cache == NULL || key == NULL || value == NULL || key_size == 0)
                return LRU_ERROR_INVALID_ARG;

        if (cache->thread_safe && pthread_rwlock_wrlock (&cache->lock) != 0)
                return LRU_ERROR_LOCK;

        node = find_in_hash_table (cache, key, key_size, NULL);

        if (node == NULL)
        {
                if (cache->track_stats)
                        cache->stats.misses++;

                result = LRU_ERROR_NOT_FOUND;
                goto cleanup;
        }

        move_to_front (cache, node);

        *value = cache->allocator.copy_fn (node->value, node->value_size);
        if (*value == NULL)
        {
                result = LRU_ERROR_NOMEM;
                goto cleanup;
        }

        if (value_size != NULL)
                *value_size = node->value_size;

        if (cache->track_stats)
                cache->stats.hits++;

        result = LRU_SUCCESS;

      cleanup:
        if (cache->thread_safe)
                pthread_rwlock_unlock (&cache->lock);

        return result;
}

int
lru_cache_peek (lru_cache_t *cache, const void *key, size_t key_size,
                void **value, size_t *value_size)
{
        lru_node_t *node;
        int     result;

        if (cache == NULL || key == NULL || value == NULL || key_size == 0)
                return LRU_ERROR_INVALID_ARG;

        if (cache->thread_safe && pthread_rwlock_rdlock (&cache->lock) != 0)
                return LRU_ERROR_LOCK;

        node = find_in_hash_table (cache, key, key_size, NULL);

        if (node == NULL)
        {
                result = LRU_ERROR_NOT_FOUND;
                goto cleanup;
        }

        *value = cache->allocator.copy_fn (node->value, node->value_size);
        if (*value == NULL)
        {
                result = LRU_ERROR_NOMEM;
                goto cleanup;
        }

        if (value_size != NULL)
                *value_size = node->value_size;

        result = LRU_SUCCESS;

      cleanup:
        if (cache->thread_safe)
                pthread_rwlock_unlock (&cache->lock);

        return result;
}

int
lru_cache_delete (lru_cache_t *cache, const void *key, size_t key_size)
{
        lru_node_t *node;
        int     result;

        if (cache == NULL || key == NULL || key_size == 0)
                return LRU_ERROR_INVALID_ARG;

        if (cache->thread_safe && pthread_rwlock_wrlock (&cache->lock) != 0)
                return LRU_ERROR_LOCK;

        node = find_in_hash_table (cache, key, key_size, NULL);

        if (node == NULL)
        {
                result = LRU_ERROR_NOT_FOUND;
                goto cleanup;
        }

        remove_from_hash_table (cache, node);
        remove_from_list (cache, node);

        cache->size--;

        if (cache->track_stats)
        {
                cache->stats.deletions++;
                cache->stats.current_size = cache->size;
        }

        destroy_node (cache, node);
        result = LRU_SUCCESS;

      cleanup:
        if (cache->thread_safe)
                pthread_rwlock_unlock (&cache->lock);

        return result;
}

bool
lru_cache_contains (lru_cache_t *cache, const void *key, size_t key_size)
{
        lru_node_t *node;
        bool    result;

        if (cache == NULL || key == NULL || key_size == 0)
                return false;

        if (cache->thread_safe && pthread_rwlock_rdlock (&cache->lock) != 0)
                return false;

        node = find_in_hash_table (cache, key, key_size, NULL);
        result = (node != NULL);

        if (cache->thread_safe)
                pthread_rwlock_unlock (&cache->lock);

        return result;
}

void
lru_cache_clear (lru_cache_t *cache)
{
        lru_node_t *node;
        lru_node_t *next;
        size_t  i;

        if (cache == NULL)
                return;

        if (cache->thread_safe)
                pthread_rwlock_wrlock (&cache->lock);

        node = cache->head;
        while (node != NULL)
        {
                next = node->next;
                destroy_node (cache, node);
                node = next;
        }

        for (i = 0; i < cache->hash_table_size; i++)
        {
                lru_hash_entry_t *entry;
                lru_hash_entry_t *next_entry;

                entry = cache->hash_table[i];
                while (entry != NULL)
                {
                        next_entry = entry->next;
                        cache->allocator.free_fn (entry);
                        entry = next_entry;
                }
                cache->hash_table[i] = NULL;
        }

        cache->head = NULL;
        cache->tail = NULL;
        cache->size = 0;

        if (cache->track_stats)
                cache->stats.current_size = 0;

        if (cache->thread_safe)
                pthread_rwlock_unlock (&cache->lock);
}

void
lru_cache_destroy (lru_cache_t *cache)
{
        if (cache == NULL)
                return;

        lru_cache_clear (cache);

        free (cache->hash_table);

        if (cache->thread_safe)
                pthread_rwlock_destroy (&cache->lock);

        free (cache);
}

size_t
lru_cache_size (lru_cache_t *cache)
{
        size_t  size;

        if (cache == NULL)
                return 0;

        if (cache->thread_safe)
                pthread_rwlock_rdlock (&cache->lock);

        size = cache->size;

        if (cache->thread_safe)
                pthread_rwlock_unlock (&cache->lock);

        return size;
}

size_t
lru_cache_capacity (lru_cache_t *cache)
{
        if (cache == NULL)
                return 0;

        return cache->capacity;
}

int
lru_cache_resize (lru_cache_t *cache, size_t new_capacity)
{
        if (cache == NULL || new_capacity < LRU_CACHE_MIN_CAPACITY)
                return LRU_ERROR_INVALID_ARG;

        if (cache->thread_safe && pthread_rwlock_wrlock (&cache->lock) != 0)
                return LRU_ERROR_LOCK;

        cache->capacity = new_capacity;

        while (cache->size > cache->capacity)
                evict_lru (cache);

        if (cache->thread_safe)
                pthread_rwlock_unlock (&cache->lock);

        return LRU_SUCCESS;
}

int
lru_cache_get_stats (lru_cache_t *cache, lru_stats_t *stats)
{
        if (cache == NULL || stats == NULL)
                return LRU_ERROR_INVALID_ARG;

        if (!cache->track_stats)
                return LRU_ERROR_INVALID_ARG;

        if (cache->thread_safe && pthread_rwlock_rdlock (&cache->lock) != 0)
                return LRU_ERROR_LOCK;

        memcpy (stats, &cache->stats, sizeof (lru_stats_t));

        if (cache->thread_safe)
                pthread_rwlock_unlock (&cache->lock);

        return LRU_SUCCESS;
}

void
lru_cache_reset_stats (lru_cache_t *cache)
{
        if (cache == NULL || !cache->track_stats)
                return;

        if (cache->thread_safe)
                pthread_rwlock_wrlock (&cache->lock);

        memset (&cache->stats, 0, sizeof (lru_stats_t));
        cache->stats.current_size = cache->size;
        cache->stats.peak_size = cache->size;

        if (cache->thread_safe)
                pthread_rwlock_unlock (&cache->lock);
}

lru_iterator_t *
lru_iterator_create (lru_cache_t *cache)
{
        lru_iterator_t *iter;

        if (cache == NULL)
                return NULL;

        iter = cache->allocator.malloc_fn (sizeof (lru_iterator_t));
        if (iter == NULL)
                return NULL;

        iter->cache = cache;
        iter->current = NULL;
        iter->locked = false;

        if (cache->thread_safe)
        {
                if (pthread_rwlock_rdlock (&cache->lock) != 0)
                {
                        cache->allocator.free_fn (iter);
                        return NULL;
                }
                iter->locked = true;
        }

        iter->current = cache->head;

        return iter;
}

bool
lru_iterator_has_next (lru_iterator_t *iter)
{
        if (iter == NULL)
                return false;

        return iter->current != NULL;
}

int
lru_iterator_next (lru_iterator_t *iter, void **key, size_t *key_size,
                   void **value, size_t *value_size)
{
        lru_node_t *node;
        lru_cache_t *cache;

        if (iter == NULL || iter->current == NULL)
                return LRU_ERROR_INVALID_ARG;

        node = iter->current;
        cache = iter->cache;

        if (key != NULL)
        {
                *key = cache->allocator.copy_fn (node->key, node->key_size);
                if (*key == NULL)
                        return LRU_ERROR_NOMEM;
        }

        if (key_size != NULL)
                *key_size = node->key_size;

        if (value != NULL)
        {
                *value = cache->allocator.copy_fn (node->value,
                                                   node->value_size);
                if (*value == NULL)
                {
                        if (key != NULL && *key != NULL)
                                cache->allocator.destroy_fn (*key);
                        return LRU_ERROR_NOMEM;
                }
        }

        if (value_size != NULL)
                *value_size = node->value_size;

        iter->current = node->next;

        return LRU_SUCCESS;
}

void
lru_iterator_destroy (lru_iterator_t *iter)
{
        if (iter == NULL)
                return;

        if (iter->locked && iter->cache->thread_safe)
                pthread_rwlock_unlock (&iter->cache->lock);

        iter->cache->allocator.free_fn (iter);
}

static void
example_eviction_callback (const void *key, size_t key_size,
                           void *value, size_t value_size, void *user_data)
{
        int    *eviction_count;

        eviction_count = (int *) user_data;
        (*eviction_count)++;

        printf ("Evicting key: %.*s\n", (int) key_size, (const char *) key);
}

static void
print_cache_stats (lru_cache_t *cache)
{
        lru_stats_t stats;

        if (lru_cache_get_stats (cache, &stats) == LRU_SUCCESS)
        {
                printf ("\nCache Statistics:\n");
                printf ("  Hits:        %lu\n", stats.hits);
                printf ("  Misses:      %lu\n", stats.misses);
                printf ("  Evictions:   %lu\n", stats.evictions);
                printf ("  Insertions:  %lu\n", stats.insertions);
                printf ("  Deletions:   %lu\n", stats.deletions);
                printf ("  Collisions:  %lu\n", stats.collisions);
                printf ("  Current Size: %zu\n", stats.current_size);
                printf ("  Peak Size:   %zu\n", stats.peak_size);

                if (stats.hits + stats.misses > 0)
                {
                        double  hit_rate;
                        hit_rate =
                                (double) stats.hits / (stats.hits +
                                                       stats.misses) * 100.0;
                        printf ("  Hit Rate:    %.2f%%\n", hit_rate);
                }
        }
}

int
main (void)
{
        lru_cache_t *cache;
        int     eviction_count;
        char   *value;
        size_t  value_size;
        int     i;
        char    key_buf[32];
        char    value_buf[64];

        printf ("=================================\n\n");

        cache = lru_cache_create (5);
        if (cache == NULL)
        {
                fprintf (stderr, "Failed to create cache\n");
                return 1;
        }

        printf ("Created cache with capacity: %zu\n",
                lru_cache_capacity (cache));

        eviction_count = 0;
        lru_cache_set_eviction_callback (cache, example_eviction_callback,
                                         &eviction_count);

        printf ("\nAdding entries to cache:\n");
        for (i = 1; i <= 7; i++)
        {
                snprintf (key_buf, sizeof (key_buf), "key%d", i);
                snprintf (value_buf, sizeof (value_buf), "value_%d", i);

                printf ("  Put: %s -> %s\n", key_buf, value_buf);
                lru_cache_put (cache, key_buf, strlen (key_buf) + 1,
                               value_buf, strlen (value_buf) + 1);
        }

        printf ("\nCache size: %zu / %zu\n",
                lru_cache_size (cache), lru_cache_capacity (cache));
        printf ("Total evictions: %d\n", eviction_count);

        printf ("\nTesting retrieval:\n");

        if (lru_cache_get (cache, "key3", 5, (void **) &value, &value_size)
            == LRU_SUCCESS)
        {
                printf ("  Get key3: %s\n", value);
                free (value);
        }
        else
        {
                printf ("  Get key3: NOT FOUND (evicted)\n");
        }

        if (lru_cache_get (cache, "key6", 5, (void **) &value, &value_size)
            == LRU_SUCCESS)
        {
                printf ("  Get key6: %s\n", value);
                free (value);
        }
        else
        {
                printf ("  Get key6: NOT FOUND\n");
        }

        printf ("\nIterating through cache (MRU to LRU):\n");
        {
                lru_iterator_t *iter;
                char   *key;
                size_t  key_size;

                iter = lru_iterator_create (cache);
                if (iter != NULL)
                {
                        while (lru_iterator_has_next (iter))
                        {
                                if (lru_iterator_next
                                    (iter, (void **) &key, &key_size,
                                     (void **) &value,
                                     &value_size) == LRU_SUCCESS)
                                {
                                        printf ("  %s -> %s\n", key, value);
                                        free (key);
                                        free (value);
                                }
                        }
                        lru_iterator_destroy (iter);
                }
        }

        print_cache_stats (cache);

        printf ("\nResizing cache to capacity 3:\n");
        lru_cache_resize (cache, 3);
        printf ("New size: %zu / %zu\n",
                lru_cache_size (cache), lru_cache_capacity (cache));
        printf ("Total evictions: %d\n", eviction_count);

        lru_cache_destroy (cache);

        printf ("\nCache destroyed successfully.\n");

        return 0;
}
