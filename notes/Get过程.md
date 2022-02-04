# LevelDB Get过程

## 1 Introduction

  在db.h 文件中定义了虚基类

```
include <stdint.h>
#include <stdio.h>
#include "leveldb/iterator.h"
#include "leveldb/options.h"
namespace leveldb {
struct Options;
struct ReadOptions;
struct WriteOptions;
class WriteBatch；
// 数据库特定状态的抽象句柄。
// 快照是不可变的对象，
// 因此可以从多个线程安全地访问，而无需任何外部同步。
class Snapshot {
 protected:
 virtual ~Snapshot();
};
// key的范围 前闭后开区间
// A range of keys
struct Range {
 Slice start;          // Included in the range
 Slice limit;          // Not included in the range
 Range() { }
 Range(const Slice& s, const Slice& l) : start(s), limit(l) { }
};
// DB是一个持久存储的 map 
// DB是一个线程安全的，不需要额外的同步
class DB {
 public:
// 用特定的name打开数据库
// 用一个堆变量（存储在堆上的变量？）指针并返回
// 成功是返回OK
// 如果失败 返回null 和空指针 
// 调用者会应当删除dbptr 如果不再需要
 static Status Open(const Options& options,
 const std::string& name,
 DB** dbptr);

 DB() { }
 virtual ~DB();
// Note:考虑设置 option.sync = ture
 virtual Status Put(const WriteOptions& options,
 const Slice& key,
 const Slice& value) = 0;
// 删除entry 
// 如果成功 ok 如果不成功 存在error 如果有问题 返回error
// note： 考虑设置option.sync = true
 virtual Status Delete(const WriteOptions& options, const Slice& key) = 0;
 // 设置数据库的更新的
 // 成功返回陈工，不ok 失败 
 // 考虑: 设置options.sync = true
 virtual Status Write(const WriteOptions& options, WriteBatch* updates) = 0;
 // 如果找得到 会放到指针那里 并且返回status ok 如果没有会返回 status isnotfound
 // 也许会返回错误
 virtual Status Get(const ReadOptions& options,
 const Slice& key, std::string* value) = 0;
 virtual Iterator* NewIterator(const ReadOptions& options) = 0;
 virtual const Snapshot* GetSnapshot() = 0;
// 释放以前的快照可以获取快照 这个caller 调用之后 后面的就没有用了
 virtual void ReleaseSnapshot(const Snapshot* snapshot) = 0;
 virtual bool GetProperty(const Slice& property, std::string* value) = 0;
 virtual void GetApproximateSizes(const Range* range, int n,
 uint64_t* sizes) = 0;
// 压缩key range存储
// 如果需要删除已经被写入的
 virtual void CompactRange(const Slice* begin, const Slice* end) = 0;
 private:
 // No copying allowed
 DB(const DB&);
 void operator=(const DB&);
};
// 摧毁特定的数据库 小心使用
Status DestroyDB(const std::string& name, const Options& options);
// 如果DB 没有被打开 也许需要调用这个命令 重建数据库
Status RepairDB(const std::string& dbname, const Options& options);
}  // namespace leveld
```

## 2 具体实现

*流程如下*

- 构造lookupKey 

- 按照顺序查找
  
  - 先查mem table
  
  - 然后imm
  
  - 最后sst文件
    
    - 查找sst文件 从level0 开始找 。 在levelDB 中除了level0 ，其他level都是没有重叠的区间的，所以可以保证从小的level找到之后，就不需要再找大的level。
    
    - 再level0 中寻找 需要查找所有区间包含key的文件，因为存在重叠区间
    
    - 再除了level0的其他层，会对level所有文件进行二分查找。
    
    - 然后读取table cache
  
  - 如果查找sst文件，且sst的文件allowed_seek <= 0 可能触发compaction 。这个allowed-seek会被查一次减少一次。

```
// 从sst读取kv
Status Version::Get(const ReadOptions& options,
 const LookupKey& k,
 std::string* value,
 GetStats* stats) {
 // ikjey internal-key
 Slice ikey = k.internal_key();
 Slice user_key = k.user_key();
 // vset 
const Comparator* ucmp = vset_->icmp_.user_comparator();
 Status s;

stats->seek_file = NULL;
 stats->seek_file_level = -1;
 FileMetaData* last_file_read = NULL;
 int last_file_read_level = -1;

// We can search level-by-level since entries never hop across
 // levels.  Therefore we are guaranteed that if we find data
 // in an smaller level, later levels are irrelevant.
 // 我们能够一层一层的搜索 因为条目永远不会跨级别 
// 因此可以保证 如果在比较 小的级别上找到数据 后面的级别就无关紧要了
 std::vector<FileMetaData*> tmp;
 FileMetaData* tmp2;
 // 每一个level都会查找一下
 for (int level = 0; level < config::kNumLevels; level++) {

size_t num_files = files_[level].size();
 if (num_files == 0) continue;

// Get the list of files to search in this level
 FileMetaData* const* files = &files_[level][0];
 // level 0查 除了level0 其他的都是没有重叠且有序的
 if (level == 0) {
 // level-0 文件也许存在重叠  找到所有重叠的user -key 并从新到旧的顺序查找重叠user key
 // Level-0 files may overlap each other.  Find all files that
 // overlap user_key and process them in order from newest to oldest.
 tmp.reserve(num_files);
 // 临时变量存 所有有可能的的文件
 for (uint32_t i = 0; i < num_files; i++) {
 FileMetaData* f = files[i];
 if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0 &&
 ucmp->Compare(user_key, f->largest.user_key()) <= 0) {
 tmp.push_back(f);
 }
 }
 if (tmp.empty()) continue;
 // 根据版本从新到老排列
 std::sort(tmp.begin(), tmp.end(), NewestFirst);
 files = &tmp[0];
 // 有多少个file
 num_files = tmp.size();
 } else {
 // Binary search to find earliest index whose largest key >= ikey.
 // 二分查找查找 看最早且最大key 大于ikey的结果
 uint32_t index = FindFile(vset_->icmp_, files_[level], ikey);
 // 就是比整个level中最大的key都大
 if (index >= num_files) {
 files = NULL;
 num_files = 0;
 } else {
 // 是否比整个level中最小的key都小
 tmp2 = files[index];
 if (ucmp->Compare(user_key, tmp2->smallest.user_key()) < 0) {
 // All of "tmp2" is past any data for user_key
 files = NULL;
 num_files = 0;
 } else {
 files = &tmp2;
 num_files = 1;
 }
 }
 }

for (uint32_t i = 0; i < num_files; ++i) {
 if (last_file_read != NULL && stats->seek_file == NULL) {
 // We have had more than one seek for this read.  Charge the 1st file.
 stats->seek_file = last_file_read;
 stats->seek_file_level = last_file_read_level;
 }

FileMetaData* f = files[i];
 last_file_read = f;
 last_file_read_level = level;

Saver saver;
 saver.state = kNotFound;
 saver.ucmp = ucmp;
 saver.user_key = user_key;
 saver.value = value;
 // 先找table cache 
s = vset_->table_cache_->Get(options, f->number, f->file_size,
 ikey, &saver, SaveValue);
 if (!s.ok()) {
 return s;
 }
 switch (saver.state) {
 case kNotFound:
 break;      // Keep searching in other files
 case kFound:
 return s;
 case kDeleted:
 s = Status::NotFound(Slice());  // Use empty error message for speed
 return s;
 case kCorrupt:
 s = Status::Corruption("corrupted key for ", user_key);
 return s;
 }
 }
 }

return Status::NotFound(Slice());  // Use an empty error message for speed
}
```
