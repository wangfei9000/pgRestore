# This is a parse table file page Python script.
# 页布局说明 https://www.postgresql.org/docs/current/storage-page-layout.html

import struct
import re

# Press the green button in the gutter to run the script.   Q 8 I 4 H 2 B 1
if __name__ == '__main__':
    columnArray = ['int8',8,'char',8,'int',4]
    tupleDataPath = "/Users/edy/24599"
    print(tupleDataPath)
    page_header_size = 24 # 页头长度是24
    with open(tupleDataPath,"rb") as f:
        data = f.read(8192)

    # PageHeaderData Layout
    pd_lsn,pd_checksum,pd_flags,pd_lower,pd_upper,pd_special,pd_pagesize_version,pd_prune_xid = struct.unpack_from("<QHHHHHHI", data, 0)
    print(f'PageHeaderData pd_lsn = [',hex(pd_lsn),'] pd_lower = [',hex(pd_lower),'] pd_upper = [',hex(pd_upper),'] pd_prune_xid = [',hex(pd_prune_xid),']')
    print('------------------------------------------')

    # ItemPointer Layout
    item_pointer = []
    item_pointer1 = []
    item_count = int((pd_lower - page_header_size) / 4)  # ItemPointer 每个4byte
    for i in range(item_count):
        offset = page_header_size + i * 4
        item, = struct.unpack_from("<I", data, offset)
        item1 = format(item, '032b')
        item_pointer += [[item1[:15], item1[15:17], item1[17:]]]
        item_pointer1 += [[hex(int(item1[:15], 2)), item1[15:17], hex(int(item1[17:], 2))]]
    print('ItemPointer : ', item_pointer1)
    print('------------------------------------------')

    # HeapTupleHeaderData Layout
    dataLen = int((int(item_pointer[0][2],2) - pd_upper) / (len(item_pointer)-1))
    for item in item_pointer:
        t_xmin,t_xmax,t_cid,t_xvac,t_ctid,t_ctid2,t_infomask2,t_infomask,t_hoff, = struct.unpack_from("<IIIIIHHHB", data, int(item[2],2))
        data1 = data[int(item[2],2)+t_infomask2  :int(item[2],2)+t_infomask2+dataLen]
        row1 = ""
        ii = 0
        for idx in range(0, len(columnArray), 2):
            if columnArray[idx] == 'int8':
                row1 += f"\t{int.from_bytes(data1[ii:ii+columnArray[idx+1]], 'little')}"
            if columnArray[idx] == 'char':
                row1 += f"\t{re.sub(r'[\x00-\x1F]+','',data1[ii:ii + columnArray[idx + 1]].decode("utf-8"))}"
            if columnArray[idx] == 'int':
                row1 += f"\t{int.from_bytes(data1[ii:ii+columnArray[idx+1]], 'little')}"
            ii += columnArray[idx+1]
        row1 += f"\t {t_xmin}/{t_xmax}"
        print(f'=>' , row1)

# 输出 -----------------------------------
/Users/edy/24599
PageHeaderData pd_lsn = [ 0x19f46a000000000 ] pd_lower = [ 0x28 ] pd_upper = [ 0x1f40 ] pd_prune_xid = [ 0x302 ]
------------------------------------------
ItemPointer :  [['0x2c', '01', '0x1fd0'], ['0x2c', '01', '0x1fa0'], ['0x2c', '01', '0x1f70'], ['0x2c', '01', '0x1f40']]
------------------------------------------
=>  1   aaaa    11   768/770
=>  2   bbb     22   769/0
=>  1   ccc     11   770/771
=>  1   dddd    11   771/0




# query database 
wf=# select * from stu2;
 id | namet | aget 
----+-------+------
  2 | bbb   |   22
  1 | dddd  |   11
(2 rows)





update stu2 set namet='dddd' where id=1;

00000000   00 00 00 00  A0 46 9F 01  00 00 00 00  28 00 40 1F  .....F......(.@.
00000010   00 20 04 20  02 03 00 00  D0 9F 58 00  A0 9F 58 00  . . ......X...X.
00000020   70 9F 58 00  40 9F 58 00  00 00 00 00  00 00 00 00  p.X.@.X.........

00001F40   03 03 00 00  00 00 00 00  00 00 00 00  00 00 00 00  ................
00001F50   04 00 03 80  02 28 18 00  01 00 00 00  00 00 00 00  .....(..........
00001F60   0D 64 64 64  64 20 00 00  0B 00 00 00  00 00 00 00  .dddd ..........

00001F70   02 03 00 00  03 03 00 00  00 00 00 00  00 00 00 00  ................
00001F80   04 00 03 C0  02 21 18 00  01 00 00 00  00 00 00 00  .....!..........
00001F90   0D 63 63 63  20 20 00 00  0B 00 00 00  00 00 00 00  .ccc  ..........

00001FA0   01 03 00 00  00 00 00 00  00 00 00 00  00 00 00 00  ................
00001FB0   02 00 03 00  02 09 18 00  02 00 00 00  00 00 00 00  ................
00001FC0   0D 62 62 62  20 20 00 00  16 00 00 00  00 00 00 00  .bbb  ..........
00001FD0   00 03 00 00  02 03 00 00  00 00 00 00  00 00 00 00  ................
00001FE0   03 00 03 40  02 05 18 00  01 00 00 00  00 00 00 00  ...@............
00001FF0   0D 61 61 61  61 20 00 00  0B 00 00 00  00 00 00 00  .aaaa ..........
00002000










