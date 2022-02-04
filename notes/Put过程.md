# Put和Delete过程

## 1 Put过程 Introduction



Put是写入



Put的流程

- 创建一个writer  writer包含一个writebatch

- writer会存在一个队列  一次只允许一个writer工作

- writer被 条件变量唤醒之后 

- MakeRoomForWriter 看是有空间放下插入的internalKey

- 会调用BuildBatchGroup 调整每个batch的大小，所以说其实单个write也是write batch

- 最后可能触发 compaction

```
Status DBImpl::Write(const WriteOptions& options, WriteBatch* my_batch) {
  // write
  Writer w(&mutex_);
  w.batch = my_batch;
  w.sync = options.sync;
  w.done = false;

  // 锁
  MutexLock l(&mutex_);
  // writers 加速w
  // 这是一个write 的deque
  writers_.push_back(&w);
  // w.done 
  // 这里应该是条件变量 等到自己是writer的队列的头部··
  while (!w.done && &w != writers_.front()) {
    // condition variable
    w.cv.Wait();
  }
  if (w.done) {
    return w.status;
  }
  // May temporarily unlock and wait.
  Status status = MakeRoomForWrite(my_batch == NULL);
  // 找空间 找到最后的sequence 
  uint64_t last_sequence = versions_->LastSequence();
  Writer* last_writer = &w;

  if (status.ok() && my_batch != NULL) {  // NULL batch is for compactions  
    WriteBatch* updates = BuildBatchGroup(&last_writer);
    WriteBatchInternal::SetSequence(updates, last_sequence + 1);
    // 一个batch 里面有多个 entry 所以要加
    last_sequence += WriteBatchInternal::Count(updates);
    // Add to log and apply to memtable.  We can release the lock
    // during this phase since &w is currently responsible for logging
    // and protects against concurrent loggers and concurrent writes
    // into mem_.
    // 在log中添加 并更新memtable  
    // 可以释放所  因为 要顺序写log 并保护log(WAL)
    // 可以并发的写道memtable中
    {
      // 这里解锁因该是方便 读这些操作
      mutex_.Unlock();
      status = log_->AddRecord(WriteBatchInternal::Contents(updates));
      bool sync_error = false;
      if (status.ok() && options.sync) {
        status = logfile_->Sync();
        if (!status.ok()) {
          sync_error = true;
        }
      }
      if (status.ok()) {
        status = WriteBatchInternal::InsertInto(updates, mem_);
      }
      mutex_.Lock();
      if (sync_error) {
        // The state of the log file is indeterminate: the log record we
        // just added may or may not show up when the DB is re-opened.
        // So we force the DB into a mode where all future writes fail.
        RecordBackgroundError(status);
      }
    }
    if (updates == tmp_batch_) tmp_batch_->Clear();

    versions_->SetLastSequence(last_sequence);
  }

  while (true) {
    // 取对头
    Writer* ready = writers_.front();
    // 把当前这个pop
    writers_.pop_front();
    // 唤醒所有准备的writer
    if (ready != &w) {
      ready->status = status;
      ready->done = true;
      // 唤醒新的
      ready->cv.Signal();
    }
    if (ready == last_writer) break;
  }
  // 条件变量通知
  if (!writers_.empty()) {
    writers_.front()->cv.Signal();
  }

  return status;
}

```



## 2 MakeRoomForWrite

主要是用于看是否存在空间

```
// REQUIRES: mutex_ is held
// REQUIRES: this thread is currently at the front of the writer queue
// 需要： 保持mutex_
// 需要： 这个线程是当前writer队列的第一个
Status DBImpl:: MakeRoomForWrite(bool force) {
  mutex_.AssertHeld();
  assert(!writers_.empty());
  bool allow_delay = !force;
  Status s;
  while (true) {
    if (!bg_error_.ok()) {
      // Yield previous error
      s = bg_error_;
      break;
    } else if (
        allow_delay &&
        versions_->NumLevelFiles(0) >= config::kL0_SlowdownWritesTrigger) {
      // We are getting close to hitting a hard limit on the number of
      // L0 files.  Rather than delaying a single write by several
      // seconds when we hit the hard limit, start delaying each
      // individual write by 1ms to reduce latency variance.  Also,
      // this delay hands over some CPU to the compaction thread in
      // case it is sharing the same core as the writer.
      // 我们会严格控制 l0的文件数量
      // 如果到了数目会delay 1ms给所有的writebatch 而不是推迟一个writebatch。以给
      // compacton 更多的cpu
      mutex_.Unlock();
      env_->SleepForMicroseconds(1000);
      allow_delay = false;  // Do not delay a single write more than once
      mutex_.Lock();
    } else if (!force &&
               (mem_->ApproximateMemoryUsage() <= options_.write_buffer_size)) {
      // There is room in current memtable
      break;
    } else if (imm_ != NULL) {
      // We have filled up the current memtable, but the previous
      // one is still being compacted, so we wait.
      // 如果imm不是null 说明其哪一个还在compact 所以等一等
      Log(options_.info_log, "Current memtable full; waiting...\n");
      bg_cv_.Wait();
    } else if (versions_->NumLevelFiles(0) >= config::kL0_StopWritesTrigger) {
      // There are too many level-0 files.
      Log(options_.info_log, "Too many L0 files; waiting...\n");
      bg_cv_.Wait();
    } else {
      // Attempt to switch to a new memtable and trigger compaction of old
      // 尝试new一个memtable并触发compact old memtable的compaction
      assert(versions_->PrevLogNumber() == 0);
      uint64_t new_log_number = versions_->NewFileNumber();
      WritableFile* lfile = NULL;
      // 要new 一个新的writablefile
      s = env_->NewWritableFile(LogFileName(dbname_, new_log_number), &lfile);
      // 如果出问题了
      if (!s.ok()) {
        // Avoid chewing through file number space in a tight loop.
        // 避免不停咬住所有同一个number space
        versions_->ReuseFileNumber(new_log_number);
        break;
      }
      delete log_;
      delete logfile_;
      logfile_ = lfile;
      logfile_number_ = new_log_number;
      // log_是日志的
      log_ = new log::Writer(lfile);
      // mem_变成
      imm_ = mem_;
      // 
      has_imm_.Release_Store(imm_);
      // mem_
      mem_ = new MemTable(internal_comparator_);
      mem_->Ref();
      force = false;   // Do not force another compaction if have room
      MaybeScheduleCompaction();
    }
  }
  return s;
}

```

这里看出如果直接没啥问题就插入。

如果有问题 

- 如果allow_delay 且level 0 存在compaction 会进行等待1000毫秒 让cpu给compaction 但是只能被dealy 一次

- 如果 有位置 直接返回 break

- 如果imm_ 部位NUll 说明现在还有个在压缩，就等着 条件变量释放·

- 最后的情况说明需要创建一个新的mem

        这里生成一个新的writeable file的log

        生成新的logname

        删除旧的log 改成新的log

        并对新mem_Ref

## 3 Delete过程

其实Delete过程就是key+kTypeDeletion

```

```




