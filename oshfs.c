#define FUSE_USE_VERSION 26
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fuse.h>
#include <sys/mman.h>

#define BLOCKSIZE 65536
#define BLOCKNR 65536
//将4G内存切分为每个block 64K，一共 64K 个block
#define MAX_BLOCK_NUM 16280 //单个文件最大支持块数
struct filenode {
    char filename[256];
    int32_t content[MAX_BLOCK_NUM];//16280*64k=1017.5MB
    int32_t filelen;//文件占用块数
    int32_t mem_num;
    struct stat st;//144字节
    struct filenode *next;
};//共64k
int now_block=0;
static const size_t size = 4 * 1024 * 1024 * (size_t)1024;
static void *mem[BLOCKNR];
int blank_block=BLOCKNR;

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
{//delete data blocks
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
{//allocate new data blocks
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
static struct filenode *get_filenode(const char *name)
{
    struct filenode *node = (struct filenode *)mem[0];
    while(node) {
        if(strcmp(node->filename, name + 1) != 0)
            node = node->next;
        else
            return node;
    }
    return NULL;
}

static int create_filenode(const char *filename, const struct stat *st)
{//创建新的文件节点，初始化文件节点的属性
    int block_avail,ret;
    if(blank_block==0){
	    ret=-ENOSPC;
        return ret;//内存空间不足
    }
    block_avail=find_block();
    mem[block_avail]=mmap(NULL,BLOCKSIZE,PROT_READ | PROT_WRITE,MAP_PRIVATE | MAP_ANONYMOUS,-1,0);
    blank_block--;
    struct filenode *newnode=(struct filenode*)mem[block_avail];
    memcpy(newnode->filename,filename,strlen(filename)+1);
    memcpy(&(newnode->st),st,sizeof(struct stat));
    newnode->filelen=0;
    newnode->mem_num=block_avail;
    struct filenode *root = (struct filenode *)mem[0];
    if(root->next==NULL)
        root->next=newnode;
    else
    {
        newnode->next=root->next;
        root->next=newnode;
    }
}

static void *oshfs_init(struct fuse_conn_info *conn)
{
    mem[0]=mmap(NULL,BLOCKSIZE,PROT_READ | PROT_WRITE,MAP_PRIVATE | MAP_ANONYMOUS,-1,0);
    struct filenode *newnode=(struct filenode *)mem[0];
    strcpy(newnode->filename,"root");
    newnode->mem_num=0;
    newnode->next=NULL;
    return NULL;
}

static int oshfs_getattr(const char *path, struct stat *stbuf)//reuse
{
    int ret = 0;
    struct filenode *node = get_filenode(path);
    if(strcmp(path, "/") == 0) 
    {
        memset(stbuf, 0, sizeof(struct stat));
        stbuf->st_mode = S_IFDIR | 0755;
    } 
    else if(node) 
    {
        memcpy(stbuf, &(node->st), sizeof(struct stat));
    } 
    else 
    {
        ret = -ENOENT;
    }
    return ret;
}

static int oshfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
    struct filenode *node = (struct filenode *)mem[0];
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    while(node) {
        filler(buf, node->filename, &(node->st), 0);
        node = node->next;
    }
    return 0;
}

static int oshfs_mknod(const char *path, mode_t mode, dev_t dev)
{
    struct stat st;
    st.st_mode = S_IFREG | 0644;
    st.st_uid = fuse_get_context()->uid;
    st.st_gid = fuse_get_context()->gid;
    st.st_nlink = 1;
    st.st_size = 0;
    create_filenode(path + 1, &st);
    return 0;
}

static int oshfs_open(const char *path, struct fuse_file_info *fi)
{//reuse
    return 0;
}

static int oshfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    struct filenode *node = get_filenode(path);
    if(node==NULL)
        return -ENOENT;
    int block_num,block_offset,start_node,rewrite=0;
    if(offset + size >= node->st.st_size)//重写offset之后所有内容,可能需要新增块
    {  
        node->st.st_size = offset + size;//新大小
        int need_block=(node->st.st_size-1)/BLOCKSIZE+1;
        if (need_block>node->filelen)//分配新增块
        {   
            int ret=allocate_newblock(node,need_block-node->filelen);
            if (!ret)
                return -ENOSPC;
            else if (ret==1)
                return -EFBIG;
        }
    }
    block_num=offset/BLOCKSIZE;//块偏移
    block_offset=offset%BLOCKSIZE;//块内偏移
    while (size>rewrite)
    { 
        start_node=node->content[block_num];
        if (BLOCKSIZE-block_offset<(size-rewrite))
        {
            memcpy(mem[start_node]+block_offset,buf+rewrite,BLOCKSIZE-block_offset);
            rewrite=BLOCKSIZE-block_offset;
            block_num++;
            block_offset=0;
        }
        else
        {
            memcpy(mem[start_node]+block_offset,buf+rewrite,size-rewrite);
            rewrite=size;
        }
    }
}
static int oshfs_truncate(const char *path, off_t size)//change the size of the file
{
    struct filenode *node = get_filenode(path);
    if(node==NULL)
        return -ENOENT;
    node->st.st_size = size;
    int filelen=(size-1)/BLOCKSIZE+1;
    if (filelen>node->filelen)
    {
        allocate_newblock(node,filelen-node->filelen);
    }
    else if (filelen<node->filelen)
    {
        delete_block(node,node->filelen-filelen);
    }
    return 0;
}

static int oshfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    struct filenode *node = get_filenode(path);
    if(node==NULL)
        return -ENOENT;
    int ret = size,block_num,block_offset,outted=0,start_node;
    if(offset + size > node->st.st_size)
        ret = node->st.st_size - offset;
    block_num=offset/BLOCKSIZE;//块偏移
    block_offset=offset%BLOCKSIZE;//块内偏移
    while (ret>outted)
    { 
        start_node=node->content[block_num];
        if (BLOCKSIZE-block_offset<(ret-outted))
        {
            memcpy(buf+outted,mem[start_node]+block_offset,BLOCKSIZE-block_offset);
            outted=BLOCKSIZE-block_offset;
            block_num++;
            block_offset=0;
        }
        else
        {
            memcpy(buf+outted,mem[start_node]+block_offset,ret-outted);
            outted=ret;
        }
    }
    return ret;
}

static int oshfs_unlink(const char *path)
{
    struct filenode *node=get_filenode(path);
    if (node==NULL)
        return -ENOENT;
    struct filenode *p=(struct filenode *)mem[0];
    while(p)
    {
        if (p->next==node)
        {   
	    if(node->next!=NULL)p->next=node->next;
	    else p->next=NULL;
	}
        else
            p=p->next;
    }
    delete_block(node,node->filelen);
    munmap(mem[node->mem_num],BLOCKSIZE);
    return 0;
}

static const struct fuse_operations op = {
    .init = oshfs_init,
    .getattr = oshfs_getattr,
    .readdir = oshfs_readdir,
    .mknod = oshfs_mknod,
    .open = oshfs_open,
    .write = oshfs_write,
    .truncate = oshfs_truncate,
    .read = oshfs_read,
    .unlink = oshfs_unlink,
};

int main(int argc, char *argv[])
{
    return fuse_main(argc, argv, &op, NULL);
}
