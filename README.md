# lru_cache
My own simple Least Recently Updated cache implementation.

## Features
- Thread-safe operations with reader-writer locks
- O(1) get, put, and delete operations
- Custom memory allocators support
- Statistics tracking
    - Hits
    - Misses
    - Evictions
- Eviction callbacks
- Iterator support for cache traversal
- Configurable capacity with dynamic resizing
- Key-value pair deep copying

## Compile and Run
- ```gcc lru_cache.c -o lru_cache```
- ```./lru_cache```


## Example result

```
=================================

Created cache with capacity: 5

Adding entries to cache:
  Put: key1 -> value_1
  Put: key2 -> value_2
  Put: key3 -> value_3
  Put: key4 -> value_4
  Put: key5 -> value_5
  Put: key6 -> value_6
Evicting key: key1
  Put: key7 -> value_7
Evicting key: key2

Cache size: 5 / 5
Total evictions: 2

Testing retrieval:
  Get key3: value_3
  Get key6: value_6

Iterating through cache (MRU to LRU):
  key6 -> value_6
  key3 -> value_3
  key7 -> value_7
  key5 -> value_5
  key4 -> value_4

Cache Statistics:
  Hits:        2
  Misses:      0
  Evictions:   2
  Insertions:  7
  Deletions:   0
  Collisions:  0
  Current Size: 5
  Peak Size:   5
  Hit Rate:    100.00%

Resizing cache to capacity 3:
Evicting key: key4
Evicting key: key5
New size: 3 / 3
Total evictions: 4

Cache destroyed successfully.
```
