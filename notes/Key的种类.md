# LevelDB的几种Key



## 1 Introduction

在LevelDB内部   一般包含3种 key 。 UserKey InternalKey  LookUpKey 这三种键



## 2 Slice

### 2.1 std::string的性能问题

std::string是基于深拷贝语义的，就是构建的对象都会有自己独自使用的一块空间

### 2.2 slice的出现

类似golang中的切片  



## 3 UserKey

userkey 基本就是一个slice   是根据用户提供的string 



## 4 InternalKey

InternalKey 主要用于数据库的内部使用 

在memory table 中的会存储 用 key 和 value 拼接的是一个 internalkey

格式如下：



UserKey + SequenceNumber+valueType



这里SequeceNumber 长8个字节， 采用边长编码  1个字节说sequence的长度，后面7个记录

value tyope 1个字节 区分是否删除

```
enum ValueType { kTypeDeletion = 0x0, kTypeValue = 0x1 };
```



## 5 LookUp Key

再上述的InternalKey 中不能区分  key 和value 的长度。所以在mem table中没办法查找。

look up key 就是 在interanlkey中加上一个varint8 记录key的长度。


