
import taos
import sys
import time
import socket
import os
import threading

from util.log import *
from util.sql import *
from util.cases import *
from util.dnodes import *
from util.common import *
sys.path.append("./7-tmq")
from tmqCommon import *

class TDTestCase:
    def init(self, conn, logSql):
        tdLog.debug(f"start to excute {__file__}")
        tdSql.init(conn.cursor())
        #tdSql.init(conn.cursor(), logSql)  # output sql.txt file

    def checkFileContent(self):
        buildPath = tdCom.getBuildPath()
        cmdStr = '%s/build/bin/sml_test'%(buildPath)
        tdLog.info(cmdStr)
        ret = os.system(cmdStr)
        if ret != 0:
            tdLog.exit("sml_test failed")

        tdSql.execute('use sml_db')
        tdSql.query("select * from t_b7d815c9222ca64cdf2614c61de8f211")
        tdSql.checkRows(1)

        tdSql.checkData(0, 0, '2016-01-01 08:00:07.000')
        tdSql.checkData(0, 1, 2000)
        tdSql.checkData(0, 2, 200)
        tdSql.checkData(0, 3, 15)
        tdSql.checkData(0, 4, 24.5208)
        tdSql.checkData(0, 5, 28.09377)
        tdSql.checkData(0, 6, 428)
        tdSql.checkData(0, 7, 0)
        tdSql.checkData(0, 8, 304)
        tdSql.checkData(0, 9, 0)
        tdSql.checkData(0, 10, 25)

        tdSql.query("select * from readings")
        tdSql.checkRows(9)

        tdSql.query("select distinct tbname from readings")
        tdSql.checkRows(4)

        tdSql.query("select * from t_0799064f5487946e5d22164a822acfc8 order by _ts")
        tdSql.checkRows(2)
        tdSql.checkData(0, 3, "kk")
        tdSql.checkData(1, 3, None)


        tdSql.query("select distinct tbname from `sys.if.bytes.out`")
        tdSql.checkRows(2)

        tdSql.query("select * from t_fc70dec6677d4277c5d9799c4da806da order by _ts")
        tdSql.checkRows(2)
        tdSql.checkData(0, 1, 1.300000000)
        tdSql.checkData(1, 1,13.000000000)

        tdSql.query("select * from `sys.procs.running`")
        tdSql.checkRows(1)
        tdSql.checkData(0, 1, 42.000000000)
        tdSql.checkData(0, 2, "web01")

        tdSql.query("select distinct tbname from `sys.cpu.nice`")
        tdSql.checkRows(2)

        tdSql.query("select * from `sys.cpu.nice` order by _ts")
        tdSql.checkRows(2)
        tdSql.checkData(0, 1, 9.000000000)
        tdSql.checkData(0, 2, "lga")
        tdSql.checkData(0, 3, "web02")
        tdSql.checkData(0, 4, None)
        tdSql.checkData(1, 1, 18.000000000)
        tdSql.checkData(1, 2, "lga")
        tdSql.checkData(1, 3, "web01")
        tdSql.checkData(1, 4, "t1")

        tdSql.query("select * from macylr")
        tdSql.checkRows(2)
        return

    def run(self):
        tdSql.prepare()
        self.checkFileContent()

    def stop(self):
        tdSql.close()
        tdLog.success(f"{__file__} successfully executed")


tdCases.addLinux(__file__, TDTestCase())
tdCases.addWindows(__file__, TDTestCase())
