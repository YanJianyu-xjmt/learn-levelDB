// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/builder.h"

#include "db/filename.h"
#include "db/dbformat.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"

namespace leveldb {


//因该是build table
Status BuildTable(const std::string& dbname,
                  Env* env,
                  const Options& options,
                  TableCache* table_cache,
                  Iterator* iter,
                  FileMetaData* meta) {
  Status s;
  meta->file_size = 0;
  // 迭代器开始使用
  iter->SeekToFirst();
  
  // 生成文件名字
  std::string fname = TableFileName(dbname, meta->number);
  if (iter->Valid()) {
    // 新文件
    WritableFile* file;
    s = env->NewWritableFile(fname, &file);
    if (!s.ok()) {
      return s;
    }
    // 这种就是builder
    TableBuilder* builder = new TableBuilder(options, file);
    // 然后meta 文件会记录最小的key 和最大的key 记录整个文件的范围
    meta->smallest.DecodeFrom(iter->key());
    // 遍历 builder 所有的
    for (; iter->Valid(); iter->Next()) {
      Slice key = iter->key();
      // 一直更新meta最大的key
      meta->largest.DecodeFrom(key);
      // BUILDER一直加
      builder->Add(key, iter->value());
    }

    // Finish and check for builder errors
    //  善后的一些工作
    if (s.ok()) {
      s = builder->Finish();
      if (s.ok()) {
        // 设置file size
        meta->file_size = builder->FileSize();
        assert(meta->file_size > 0);
      }
    } else {
      builder->Abandon();
    }
    delete builder;

    // Finish and check for file errors
    if (s.ok()) {
      s = file->Sync();
    }
    if (s.ok()) {
      s = file->Close();
    }
    delete file;
    file = NULL;

    if (s.ok()) {
      // Verify that the table is usable
      Iterator* it = table_cache->NewIterator(ReadOptions(),
                                              meta->number,
                                              meta->file_size);
      s = it->status();
      delete it;
    }
  }

  // Check for input iterator errors
  if (!iter->status().ok()) {
    s = iter->status();
  }

  if (s.ok() && meta->file_size > 0) {
    // Keep it
  } else {
    env->DeleteFile(fname);
  }
  return s;
}

}  // namespace leveldb
