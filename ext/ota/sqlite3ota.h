/*
** 2014 August 30
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
**
** This file contains the public interface for the OTA extension. 
*/

/*
** SUMMARY
**
** Writing a transaction containing a large number of operations on 
** b-tree indexes that are collectively larger than the available cache
** memory can be very inefficient. 
**
** The problem is that in order to update a b-tree, the leaf page (at least)
** containing the entry being inserted or deleted must be modified. If the
** working set of leaves is larger than the available cache memory, then a 
** single leaf that is modified more than once as part of the transaction 
** may be loaded from or written to the persistent media multiple times.
** Additionally, because the index updates are likely to be applied in
** random order, access to pages within the database is also likely to be in 
** random order, which is itself quite inefficient.
**
** One way to improve the situation is to sort the operations on each index
** by index key before applying them to the b-tree. This leads to an IO
** pattern that resembles a single linear scan through the index b-tree,
** and all but guarantees each modified leaf page is loaded and stored 
** exactly once. SQLite uses this trick to improve the performance of
** CREATE INDEX commands. This extension allows it to be used to improve
** the performance of large transactions on existing databases.
**
** Additionally, this extension allows the work involved in writing the 
** large transaction to be broken down into sub-transactions performed 
** sequentially by separate processes. This is useful if the system cannot 
** guarantee that a single update process will run for long enough to apply 
** the entire update, for example because the update is being applied on a 
** mobile device that is frequently rebooted. Even after the writer process 
** has committed one or more sub-transactions, other database clients continue
** to read from the original database snapshot. In other words, partially 
** applied transactions are not visible to other clients. 
**
** "OTA" stands for "Over The Air" update. As in a large database update
** transmitted via a wireless network to a mobile device. A transaction
** applied using this extension is hence refered to as an "OTA update".
**
**
** LIMITATIONS
**
** An "OTA update" transaction is subject to the following limitations:
**
**   * The transaction must consist of INSERT, UPDATE and DELETE operations
**     only.
**
**   * INSERT statements may not use any default values.
**
**   * UPDATE and DELETE statements must identify their target rows by 
**     non-NULL PRIMARY KEY values. Rows with NULL values stored in PRIMARY
**     KEY fields may not be updated or deleted. If the table being written 
**     has no PRIMARY KEY, affected rows must be identified by rowid.
**
**   * UPDATE statements may not modify PRIMARY KEY columns.
**
**   * No triggers will be fired.
**
**   * No foreign key violations are detected or reported.
**
**   * CHECK constraints are not enforced.
**
**   * No constraint handling mode except for "OR ROLLBACK" is supported.
**
**
** PREPARATION
**
** An "OTA update" is stored as a separate SQLite database. A database
** containing an OTA update is an "OTA database". For each table in the 
** target database to be updated, the OTA database should contain a table
** named "data_<target name>" containing the same set of columns as the
** target table, and one more - "ota_control". The data_% table should 
** have no PRIMARY KEY or UNIQUE constraints, but each column should have
** the same type as the corresponding column in the target database.
** The "ota_control" column should have no type at all. For example, if
** the target database contains:
**
**   CREATE TABLE t1(a INTEGER PRIMARY KEY, b TEXT, c UNIQUE);
**
** Then the OTA database should contain:
**
**   CREATE TABLE data_t1(a INTEGER, b TEXT, c, ota_control);
**
** The order of the columns in the data_% table does not matter.
**
** If the target database table is a virtual table or a table that has no
** PRIMARY KEY declaration, the data_% table must also contain a column 
** named "ota_rowid". This column is mapped to the tables implicit primary 
** key column - "rowid". Virtual tables for which the "rowid" column does 
** not function like a primary key value cannot be updated using OTA. For 
** example, if the target db contains either of the following:
**
**   CREATE VIRTUAL TABLE x1 USING fts3(a, b);
**   CREATE TABLE x1(a, b)
**
** then the OTA database should contain:
**
**   CREATE TABLE data_x1(a, b, ota_rowid, ota_control);
**
** All non-hidden columns (i.e. all columns matched by "SELECT *") of the
** target table must be present in the input table. For virtual tables,
** hidden columns are optional - they are updated by OTA if present in
** the input table, or not otherwise. For example, to write to an fts4
** table with a hidden languageid column such as:
**
**   CREATE VIRTUAL TABLE ft1 USING fts4(a, b, languageid='langid');
**
** Either of the following input table schemas may be used:
**
**   CREATE TABLE data_ft1(a, b, langid, ota_rowid, ota_control);
**   CREATE TABLE data_ft1(a, b, ota_rowid, ota_control);
**
** For each row to INSERT into the target database as part of the OTA 
** update, the corresponding data_% table should contain a single record
** with the "ota_control" column set to contain integer value 0. The
** other columns should be set to the values that make up the new record 
** to insert. 
**
** If the target database table has an INTEGER PRIMARY KEY, it is not 
** possible to insert a NULL value into the IPK column. Attempting to 
** do so results in an SQLITE_MISMATCH error.
**
** For each row to DELETE from the target database as part of the OTA 
** update, the corresponding data_% table should contain a single record
** with the "ota_control" column set to contain integer value 1. The
** real primary key values of the row to delete should be stored in the
** corresponding columns of the data_% table. The values stored in the
** other columns are not used.
**
** For each row to UPDATE from the target database as part of the OTA 
** update, the corresponding data_% table should contain a single record
** with the "ota_control" column set to contain a value of type text.
** The real primary key values identifying the row to update should be 
** stored in the corresponding columns of the data_% table row, as should
** the new values of all columns being update. The text value in the 
** "ota_control" column must contain the same number of characters as
** there are columns in the target database table, and must consist entirely
** of 'x' and '.' characters (or in some special cases 'd' - see below). For 
** each column that is being updated, the corresponding character is set to
** 'x'. For those that remain as they are, the corresponding character of the
** ota_control value should be set to '.'. For example, given the tables 
** above, the update statement:
**
**   UPDATE t1 SET c = 'usa' WHERE a = 4;
**
** is represented by the data_t1 row created by:
**
**   INSERT INTO data_t1(a, b, c, ota_control) VALUES(4, NULL, 'usa', '..x');
**
** Instead of an 'x' character, characters of the ota_control value specified
** for UPDATEs may also be set to 'd'. In this case, instead of updating the
** target table with the value stored in the corresponding data_% column, the
** user-defined SQL function "ota_delta()" is invoked and the result stored in
** the target table column. ota_delta() is invoked with two arguments - the
** original value currently stored in the target table column and the 
** value specified in the data_xxx table.
**
** For example, this row:
**
**   INSERT INTO data_t1(a, b, c, ota_control) VALUES(4, NULL, 'usa', '..d');
**
** is similar to an UPDATE statement such as: 
**
**   UPDATE t1 SET c = ota_delta(c, 'usa') WHERE a = 4;
**
** If the target database table is a virtual table or a table with no PRIMARY
** KEY, the ota_control value should not include a character corresponding 
** to the ota_rowid value. For example, this:
**
**   INSERT INTO data_ft1(a, b, ota_rowid, ota_control) 
**       VALUES(NULL, 'usa', 12, '.x');
**
** causes a result similar to:
**
**   UPDATE ft1 SET b = 'usa' WHERE rowid = 12;
**
** The data_xxx tables themselves should have no PRIMARY KEY declarations.
** However, OTA is more efficient if reading the rows in from each data_xxx
** table in "rowid" order is roughly the same as reading them sorted by
** the PRIMARY KEY of the corresponding target database table. In other 
** words, rows should be sorted using the destination table PRIMARY KEY 
** fields before they are inserted into the data_xxx tables.
**
** USAGE
**
** The API declared below allows an application to apply an OTA update 
** stored on disk to an existing target database. Essentially, the 
** application:
**
**     1) Opens an OTA handle using the sqlite3ota_open() function.
**
**     2) Registers any required virtual table modules with the database
**        handle returned by sqlite3ota_db(). Also, if required, register
**        the ota_delta() implementation.
**
**     3) Calls the sqlite3ota_step() function one or more times on
**        the new handle. Each call to sqlite3ota_step() performs a single
**        b-tree operation, so thousands of calls may be required to apply 
**        a complete update.
**
**     4) Calls sqlite3ota_close() to close the OTA update handle. If
**        sqlite3ota_step() has been called enough times to completely
**        apply the update to the target database, then the OTA database
**        is marked as fully applied. Otherwise, the state of the OTA 
**        update application is saved in the OTA database for later 
**        resumption.
**
** See comments below for more detail on APIs.
**
** If an update is only partially applied to the target database by the
** time sqlite3ota_close() is called, various state information is saved 
** within the OTA database. This allows subsequent processes to automatically
** resume the OTA update from where it left off.
**
** To remove all OTA extension state information, returning an OTA database 
** to its original contents, it is sufficient to drop all tables that begin
** with the prefix "ota_"
**
** DATABASE LOCKING
**
** An OTA update may not be applied to a database in WAL mode. Attempting
** to do so is an error (SQLITE_ERROR).
**
** While an OTA handle is open, a SHARED lock may be held on the target
** database file. This means it is possible for other clients to read the
** database, but not to write it.
**
** If an OTA update is started and then suspended before it is completed,
** then an external client writes to the database, then attempting to resume
** the suspended OTA update is also an error (SQLITE_BUSY).
*/

#ifndef _SQLITE3OTA_H
#define _SQLITE3OTA_H

#include "sqlite3.h"              /* Required for error code definitions */

typedef struct sqlite3ota sqlite3ota;

/*
** Open an OTA handle.
**
** Argument zTarget is the path to the target database. Argument zOta is
** the path to the OTA database. Each call to this function must be matched
** by a call to sqlite3ota_close(). When opening the databases, OTA passes
** the SQLITE_CONFIG_URI flag to sqlite3_open_v2(). So if either zTarget
** or zOta begin with "file:", it will be interpreted as an SQLite 
** database URI, not a regular file name.
**
** If the zState argument is passed a NULL value, the OTA extension stores 
** the current state of the update (how many rows have been updated, which 
** indexes are yet to be updated etc.) within the OTA database itself. This
** can be convenient, as it means that the OTA application does not need to
** organize removing a separate state file after the update is concluded. 
** Or, if zState is non-NULL, it must be a path to a database file in which 
** the OTA extension can store the state of the update.
**
** When resuming an OTA update, the zState argument must be passed the same
** value as when the OTA update was started.
**
** Once the OTA update is finished, the OTA extension does not 
** automatically remove any zState database file, even if it created it.
**
** By default, OTA uses the default VFS to access the files on disk. To
** use a VFS other than the default, an SQLite "file:" URI containing a
** "vfs=..." option may be passed as the zTarget option.
**
** IMPORTANT NOTE FOR ZIPVFS USERS: The OTA extension works with all of
** SQLite's built-in VFSs, including the multiplexor VFS. However it does
** not work out of the box with zipvfs. Refer to the comment describing
** the zipvfs_create_vfs() API below for details on using OTA with zipvfs.
*/
sqlite3ota *sqlite3ota_open(
  const char *zTarget, 
  const char *zOta,
  const char *zState
);

/*
** Internally, each OTA connection uses a separate SQLite database 
** connection to access the target and ota update databases. This
** API allows the application direct access to these database handles.
**
** The first argument passed to this function must be a valid, open, OTA
** handle. The second argument should be passed zero to access the target
** database handle, or non-zero to access the ota update database handle.
** Accessing the underlying database handles may be useful in the
** following scenarios:
**
**   * If any target tables are virtual tables, it may be necessary to
**     call sqlite3_create_module() on the target database handle to 
**     register the required virtual table implementations.
**
**   * If the data_xxx tables in the OTA source database are virtual 
**     tables, the application may need to call sqlite3_create_module() on
**     the ota update db handle to any required virtual table
**     implementations.
**
**   * If the application uses the "ota_delta()" feature described above,
**     it must use sqlite3_create_function() or similar to register the
**     ota_delta() implementation with the target database handle.
**
** If an error has occurred, either while opening or stepping the OTA object,
** this function may return NULL. The error code and message may be collected
** when sqlite3ota_close() is called.
*/
sqlite3 *sqlite3ota_db(sqlite3ota*, int bOta);

/*
** Do some work towards applying the OTA update to the target db. 
**
** Return SQLITE_DONE if the update has been completely applied, or 
** SQLITE_OK if no error occurs but there remains work to do to apply
** the OTA update. If an error does occur, some other error code is 
** returned. 
**
** Once a call to sqlite3ota_step() has returned a value other than
** SQLITE_OK, all subsequent calls on the same OTA handle are no-ops
** that immediately return the same value.
*/
int sqlite3ota_step(sqlite3ota *pOta);

/*
** Close an OTA handle. 
**
** If the OTA update has been completely applied, mark the OTA database
** as fully applied. Otherwise, assuming no error has occurred, save the
** current state of the OTA update appliation to the OTA database.
**
** If an error has already occurred as part of an sqlite3ota_step()
** or sqlite3ota_open() call, or if one occurs within this function, an
** SQLite error code is returned. Additionally, *pzErrmsg may be set to
** point to a buffer containing a utf-8 formatted English language error
** message. It is the responsibility of the caller to eventually free any 
** such buffer using sqlite3_free().
**
** Otherwise, if no error occurs, this function returns SQLITE_OK if the
** update has been partially applied, or SQLITE_DONE if it has been 
** completely applied.
*/
int sqlite3ota_close(sqlite3ota *pOta, char **pzErrmsg);

/*
** Return the total number of key-value operations (inserts, deletes or 
** updates) that have been performed on the target database since the
** current OTA update was started.
*/
sqlite3_int64 sqlite3ota_progress(sqlite3ota *pOta);

/*
** Create an OTA VFS named zName that accesses the underlying file-system
** via existing VFS zParent. Or, if the zParent parameter is passed NULL, 
** then the new OTA VFS uses the default system VFS to access the file-system.
** The new object is registered as a non-default VFS with SQLite before 
** returning.
**
** Part of the OTA implementation uses a custom VFS object. Usually, this
** object is created and deleted automatically by OTA. 
**
** The exception is for applications that also use zipvfs. In this case,
** the custom VFS must be explicitly created by the user before the OTA
** handle is opened. The OTA VFS should be installed so that the zipvfs
** VFS uses the OTA VFS, which in turn uses any other VFS layers in use 
** (for example multiplexor) to access the file-system. For example,
** to assemble an OTA enabled VFS stack that uses both zipvfs and 
** multiplexor (error checking omitted):
**
**     // Create a VFS named "multiplex" (not the default).
**     sqlite3_multiplex_initialize(0, 0);
**
**     // Create an ota VFS named "ota" that uses multiplexor. If the
**     // second argument were replaced with NULL, the "ota" VFS would
**     // access the file-system via the system default VFS, bypassing the
**     // multiplexor.
**     sqlite3ota_create_vfs("ota", "multiplex");
**
**     // Create a zipvfs VFS named "zipvfs" that uses ota.
**     zipvfs_create_vfs_v3("zipvfs", "ota", 0, xCompressorAlgorithmDetector);
**
**     // Make zipvfs the default VFS.
**     sqlite3_vfs_register(sqlite3_vfs_find("zipvfs"), 1);
**
** Because the default VFS created above includes a OTA functionality, it
** may be used by OTA clients. Attempting to use OTA with a zipvfs VFS stack
** that does not include the OTA layer results in an error.
**
** The overhead of adding the "ota" VFS to the system is negligible for 
** non-OTA users. There is no harm in an application accessing the 
** file-system via "ota" all the time, even if it only uses OTA functionality 
** occasionally.
*/
int sqlite3ota_create_vfs(const char *zName, const char *zParent);

/*
** Deregister and destroy an OTA vfs created by an earlier call to
** sqlite3ota_create_vfs().
**
** VFS objects are not reference counted. If a VFS object is destroyed
** before all database handles that use it have been closed, the results
** are undefined.
*/
void sqlite3ota_destroy_vfs(const char *zName);

#endif /* _SQLITE3OTA_H */
