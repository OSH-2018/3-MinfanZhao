# 3-MinfanZhao
## 内存文件系统

文件系统支持最多 64k 个文件，单个文件最大不超过 1017.5MB。

可以正确运行如下检测

```
$ cd mountpoint
$ ls -al
$ echo helloworld > testfile
$ ls -l testfile # 查看是否成功创建文件
$ cat testfile # 测试写入和读取是否相同
$ dd if=/dev/zero of=testfile bs=1M count=2000
$ ls -l testfile # 测试2000MiB大文件写入
$ dd if=/dev/urandom of=testfile bs=1M count=1 seek=10
$ ls -l testfile # 此时应为11MiB
$ dd if=testfile of=/dev/null # 测试文件读取
$ rm testfile
$ ls -al # testfile是否成功删除
```
测试时最高写入速度可达 450MB/s。

### block 设计

将 4GB 内存分为 64K 个 64K 大小的 block，使用一个 `*mem[BLOCKNR]` 数组来标记和指向。

```
#define BLOCKSIZE 65536
#define BLOCKNR 65536
static const size_t size = 4 * 1024 * 1024 * (size_t)1024;
static void *mem[BLOCKNR];
```

每个 block 可以有两种用途：

* 文件节点
* 数据节点

### 文件节点

``` 
#define MAX_BLOCK_NUM 16280 //单个文件最大支持块数
struct filenode {
    char filename[256];
    int32_t content[MAX_BLOCK_NUM];//16280*64k=1017.5MB
    int32_t filelen;//文件占用块数
    int32_t mem_num;
    struct stat st;//144字节
    struct filenode *next;
};//共64k
```

通过预设内容数组`content[MAX_BLOCK_NUM]`，文件节点可以访问到该文件所使用的数据节点 block，经过设计的文件节点大小也为 64k，最大支持单个文件大小为 16280*64k=1017.5MB，超过这个大小的文件写入会报错 `EFBIG`。

各文件节点之间通过链表进行链接，从 `root` 节点开始，通过链表即可访问所有文件。

各变量解释如下：

* `filename[]` 存放文件名，最大不超过 256 字节，满足 ext4 对 255 字节文件名的要求。 

* `content[]` 数组中存放的是 `*mem[]` 数组的下标，即分配给该文件节点的数据节点在内存中的位置。
* `filelen` 存放该文件节点使用的数据节点个数。
* ` mem_num` 存放该文件节点对应的 `*mem[]` 数组下标，即文件节点的位置。
* `struct stat st` 文件属性。
* ` *next ` 指向文件节点链表中的下一个节点。

### 数据节点

数据节点仅用于存放数据，不包含任何其他的信息，每个数据节点大小为 64k。

### 存储算法

通过 `blank_block` 来记录当前剩余未分配的 block 数量。

通过 `now_block` 来记录上次分配的 block 在 mem 中的位置，避免每次都从头寻找空的 block。

每次要分配新的 block 时，通过查找未分配 block，返回其在 `*mem[]` 中的下标，即可分配对应位置的 block。

在释放 block 时，将 `now_block` 位置更新，以便与下次分配时直接查找到空闲块，可以提升效率。

```
int find_block()
{//find an unused block
    int i=now_block;
    for (i=(now_block+1)%BLOCKNR;i!=now_block;i=(i+1)%BLOCKNR)
    {
        if(mem[i]==NULL)
        {
            now_block=i;
            return i;
        }
    }
    return -1;
}
void delete_block(struct filenode *node,int num)
{//delete a data block
    if (num>node->filelen)
        num=node->filelen;
    int mem_locat;
    while(num>0)
    {
    	mem_locat=node->content[node->filelen-1];
    	now_block=mem_locat>0?mem_locat-1:BLOCKNR;
        munmap(mem[mem_locat],BLOCKSIZE);
        num--;
        node->filelen--;
        blank_block++;
    }
}
int allocate_newblock(struct filenode *node,int num)
{//allocate  new data blocks
    int i;
    if(num>blank_block)
        return 0;//内存空间不足
    else if (num>(MAX_BLOCK_NUM-node->filelen))
        return 1;//达到单个文件大小上限
    else
    {
        while (num>0)
        {
            i=find_block();
            node->filelen++;
            node->content[node->filelen-1]=i;
            mem[i]=mmap(NULL,BLOCKSIZE,PROT_READ | PROT_WRITE,MAP_PRIVATE | MAP_ANONYMOUS,-1,0);
            num--;
            blank_block--;
        }  
        return 2;      //成功
    }
}
```

### 其他

选做部分未实现。
