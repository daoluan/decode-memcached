/* associative array */
// 关联数组
// 在计算机科学中，关联数组（英语：Associative Array），又称映射（Map）、字典（Dictionary）是一个抽象的数据结构，它包含着类似于（键，值）的有序对。一个关联数组中的有序对可以重复（如C++中的multimap）也可以不重复（如C++中的map）。
void assoc_init(const int hashpower_init);
item *assoc_find(const char *key, const size_t nkey, const uint32_t hv);
int assoc_insert(item *item, const uint32_t hv);
void assoc_delete(const char *key, const size_t nkey, const uint32_t hv);
void do_assoc_move_next_bucket(void);
int start_assoc_maintenance_thread(void);
void stop_assoc_maintenance_thread(void);
extern unsigned int hashpower;
