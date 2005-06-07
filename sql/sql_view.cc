/* Copyright (C) 2004 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "mysql_priv.h"
#include "sql_select.h"
#include "parse_file.h"
#include "sp.h"
#include "sp_head.h"

#define MD5_BUFF_LENGTH 33

static int mysql_register_view(THD *thd, TABLE_LIST *view,
			       enum_view_create_mode mode);

const char *updatable_views_with_limit_names[]= { "NO", "YES", NullS };
TYPELIB updatable_views_with_limit_typelib=
{
  array_elements(updatable_views_with_limit_names)-1, "",
  updatable_views_with_limit_names,
  0
};


/*
  Creating/altering VIEW procedure

  SYNOPSIS
    mysql_create_view()
    thd		- thread handler
    mode	- VIEW_CREATE_NEW, VIEW_ALTER, VIEW_CREATE_OR_REPLACE

  RETURN VALUE
     FALSE OK
     TRUE  Error
*/

bool mysql_create_view(THD *thd,
                       enum_view_create_mode mode)
{
  LEX *lex= thd->lex;
  bool link_to_local;
  /* first table in list is target VIEW name => cut off it */
  TABLE_LIST *view= lex->unlink_first_table(&link_to_local);
  TABLE_LIST *tables= lex->query_tables;
  TABLE_LIST *tbl;
  SELECT_LEX *select_lex= &lex->select_lex;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  SELECT_LEX *sl;
#endif
  SELECT_LEX_UNIT *unit= &lex->unit;
  bool res= FALSE;
  DBUG_ENTER("mysql_create_view");

  if (lex->proc_list.first ||
      lex->result)
  {
    my_error(ER_VIEW_SELECT_CLAUSE, MYF(0), (lex->result ?
                                             "INTO" :
                                             "PROCEDURE"));
    res= TRUE;
    goto err;
  }
  if (lex->derived_tables ||
      lex->variables_used || lex->param_list.elements)
  {
    int err= (lex->derived_tables ?
              ER_VIEW_SELECT_DERIVED :
              ER_VIEW_SELECT_VARIABLE);
    my_message(err, ER(err), MYF(0));
    res= TRUE;
    goto err;
  }

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  /*
    Privilege check for view creation:
    - user has CREATE VIEW privilege on view table
    - user has DROP privilege in case of ALTER VIEW or CREATE OR REPLACE
    VIEW
    - user has some (SELECT/UPDATE/INSERT/DELETE) privileges on columns of
    underlying tables used on top of SELECT list (because it can be
    (theoretically) updated, so it is enough to have UPDATE privilege on
    them, for example)
    - user has SELECT privilege on columns used in expressions of VIEW select
    - for columns of underly tables used on top of SELECT list also will be
    checked that we have not more privileges on correspondent column of view
    table (i.e. user will not get some privileges by view creation)
  */
  if ((check_access(thd, CREATE_VIEW_ACL, view->db, &view->grant.privilege,
                    0, 0) ||
       grant_option && check_grant(thd, CREATE_VIEW_ACL, view, 0, 1, 0)) ||
      (mode != VIEW_CREATE_NEW &&
       (check_access(thd, DROP_ACL, view->db, &view->grant.privilege,
                     0, 0) ||
        grant_option && check_grant(thd, DROP_ACL, view, 0, 1, 0))))
    DBUG_RETURN(TRUE);
  for (sl= select_lex; sl; sl= sl->next_select())
  {
    for (tbl= sl->get_table_list(); tbl; tbl= tbl->next_local)
    {
      /*
        Ensure that we have some privileges on this table, more strict check
        will be done on column level after preparation,
      */
      if (check_some_access(thd, VIEW_ANY_ACL, tbl))
      {
        my_error(ER_TABLEACCESS_DENIED_ERROR, MYF(0),
                 "ANY", thd->priv_user, thd->host_or_ip, tbl->table_name);
        DBUG_RETURN(TRUE);
      }
      /*
        Mark this table as a table which will be checked after the prepare
        phase
      */
      tbl->table_in_first_from_clause= 1;

      /*
        We need to check only SELECT_ACL for all normal fields, fields for
        which we need "any" (SELECT/UPDATE/INSERT/DELETE) privilege will be
        checked later
      */
      tbl->grant.want_privilege= SELECT_ACL;
      /*
        Make sure that all rights are loaded to the TABLE::grant field.

        tbl->table_name will be correct name of table because VIEWs are
        not opened yet.
      */
      fill_effective_table_privileges(thd, &tbl->grant, tbl->db,
                                      tbl->table_name);
    }
  }

  if (&lex->select_lex != lex->all_selects_list)
  {
    /* check tables of subqueries */
    for (tbl= tables; tbl; tbl= tbl->next_global)
    {
      if (!tbl->table_in_first_from_clause)
      {
        if (check_access(thd, SELECT_ACL, tbl->db,
                         &tbl->grant.privilege, 0, 0) ||
            grant_option && check_grant(thd, SELECT_ACL, tbl, 0, 1, 0))
        {
          res= TRUE;
          goto err;
        }
      }
    }
  }
  /*
    Mark fields for special privilege check ("any" privilege)
  */
  for (sl= select_lex; sl; sl= sl->next_select())
  {
    List_iterator_fast<Item> it(sl->item_list);
    Item *item;
    while ((item= it++))
    {
      Item_field *field;
      if ((field= item->filed_for_view_update()))
        field->any_privileges= 1;
    }
  }
#endif

  if (open_and_lock_tables(thd, tables))
    DBUG_RETURN(TRUE);

  /*
    check that tables are not temporary  and this VIEW do not used in query
    (it is possible with ALTERing VIEW)
  */
  for (tbl= tables; tbl; tbl= tbl->next_global)
  {
    /* is this table temporary and is not view? */
    if (tbl->table->s->tmp_table != NO_TMP_TABLE && !tbl->view &&
        !tbl->schema_table)
    {
      my_error(ER_VIEW_SELECT_TMPTABLE, MYF(0), tbl->alias);
      res= TRUE;
      goto err;
    }

    /* is this table view and the same view which we creates now? */
    if (tbl->view &&
        strcmp(tbl->view_db.str, view->db) == 0 &&
        strcmp(tbl->view_name.str, view->table_name) == 0)
    {
      my_error(ER_NO_SUCH_TABLE, MYF(0), tbl->view_db.str, tbl->view_name.str);
      res= TRUE;
      goto err;
    }

    /*
      Copy the privileges of the underlying VIEWs which were filled by
      fill_effective_table_privileges
      (they were not copied at derived tables processing)
    */
    tbl->table->grant.privilege= tbl->grant.privilege;
  }

  /* prepare select to resolve all fields */
  lex->view_prepare_mode= 1;
  if (unit->prepare(thd, 0, 0, view->view_name.str))
  {
    /*
      some errors from prepare are reported to user, if is not then
      it will be checked after err: label
    */
    res= TRUE;
    goto err;
  }

  /* view list (list of view fields names) */
  if (lex->view_list.elements)
  {
    List_iterator_fast<Item> it(select_lex->item_list);
    List_iterator_fast<LEX_STRING> nm(lex->view_list);
    Item *item;
    LEX_STRING *name;

    if (lex->view_list.elements != select_lex->item_list.elements)
    {
      my_message(ER_VIEW_WRONG_LIST, ER(ER_VIEW_WRONG_LIST), MYF(0));
      goto err;
    }
    while ((item= it++, name= nm++))
      item->set_name(name->str, name->length, system_charset_info);
  }

  /* Test absence of duplicates names */
  {
    Item *item;
    List_iterator_fast<Item> it(select_lex->item_list);
    it++;
    while ((item= it++))
    {
      Item *check;
      List_iterator_fast<Item> itc(select_lex->item_list);
      while ((check= itc++) && check != item)
      {
        if (strcmp(item->name, check->name) == 0)
        {
          my_error(ER_DUP_FIELDNAME, MYF(0), item->name);
          DBUG_RETURN(TRUE);
        }
      }
    }
  }

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  /*
    Compare/check grants on view with grants of underlying tables
  */
  for (sl= select_lex; sl; sl= sl->next_select())
  {
    char *db= view->db ? view->db : thd->db;
    List_iterator_fast<Item> it(sl->item_list);
    Item *item;
    fill_effective_table_privileges(thd, &view->grant, db,
                                    view->table_name);
    while ((item= it++))
    {
      Item_field *fld;
      uint priv= (get_column_grant(thd, &view->grant, db,
                                    view->table_name, item->name) &
                  VIEW_ANY_ACL);
      if ((fld= item->filed_for_view_update()))
      {
        /*
          Do we have more privileges on view field then underlying table field?
        */
        if (!fld->field->table->s->tmp_table && (~fld->have_privileges & priv))
        {
          /* VIEW column has more privileges */
          my_error(ER_COLUMNACCESS_DENIED_ERROR, MYF(0),
                   "create view", thd->priv_user, thd->host_or_ip, item->name,
                   view->table_name);
          DBUG_RETURN(TRUE);
        }
      }
    }
  }
#endif

  if (wait_if_global_read_lock(thd, 0, 0))
  {
    res= TRUE;
    goto err;
  }
  VOID(pthread_mutex_lock(&LOCK_open));
  res= mysql_register_view(thd, view, mode);
  VOID(pthread_mutex_unlock(&LOCK_open));
  if (view->revision != 1)
    query_cache_invalidate3(thd, view, 0);
  start_waiting_global_read_lock(thd);
  if (res)
    goto err;

  send_ok(thd);
  lex->link_first_table_back(view, link_to_local);
  DBUG_RETURN(0);

err:
  thd->proc_info= "end";
  lex->link_first_table_back(view, link_to_local);
  unit->cleanup();
  DBUG_RETURN(res || thd->net.report_error);
}


/* index of revision number in following table */
static const int revision_number_position= 5;
/* index of last required parameter for making view */
static const int required_view_parameters= 7;

/*
  table of VIEW .frm field descriptors

  Note that one should NOT change the order for this, as it's used by
  parse()
*/
static File_option view_parameters[]=
{{{(char*) "query", 5},		offsetof(TABLE_LIST, query),
  FILE_OPTIONS_STRING},
 {{(char*) "md5", 3},		offsetof(TABLE_LIST, md5),
  FILE_OPTIONS_STRING},
 {{(char*) "updatable", 9},	offsetof(TABLE_LIST, updatable_view),
  FILE_OPTIONS_ULONGLONG},
 {{(char*) "algorithm", 9},	offsetof(TABLE_LIST, algorithm),
  FILE_OPTIONS_ULONGLONG},
 {{(char*) "with_check_option", 17}, offsetof(TABLE_LIST, with_check),
   FILE_OPTIONS_ULONGLONG},
 {{(char*) "revision", 8},	offsetof(TABLE_LIST, revision),
  FILE_OPTIONS_REV},
 {{(char*) "timestamp", 9},	offsetof(TABLE_LIST, timestamp),
  FILE_OPTIONS_TIMESTAMP},
 {{(char*)"create-version", 14},offsetof(TABLE_LIST, file_version),
  FILE_OPTIONS_ULONGLONG},
 {{(char*) "source", 6},	offsetof(TABLE_LIST, source),
  FILE_OPTIONS_ESTRING},
 {{NullS, 0},			0,
  FILE_OPTIONS_STRING}
};

static LEX_STRING view_file_type[]= {{(char*)"VIEW", 4}};


/*
  Register VIEW (write .frm & process .frm's history backups)

  SYNOPSIS
    mysql_register_view()
    thd		- thread handler
    view	- view description
    mode	- VIEW_CREATE_NEW, VIEW_ALTER, VIEW_CREATE_OR_REPLACE

  RETURN
     0	OK
    -1	Error
     1	Error and error message given
*/

static int mysql_register_view(THD *thd, TABLE_LIST *view,
			       enum_view_create_mode mode)
{
  LEX *lex= thd->lex;
  char buff[4096];
  String str(buff,(uint32) sizeof(buff), system_charset_info);
  char md5[MD5_BUFF_LENGTH];
  bool can_be_merged;
  char dir_buff[FN_REFLEN], file_buff[FN_REFLEN];
  LEX_STRING dir, file;
  DBUG_ENTER("mysql_register_view");

  /* print query */
  str.length(0);
  {
    ulong sql_mode= thd->variables.sql_mode & MODE_ANSI_QUOTES;
    thd->variables.sql_mode&= ~MODE_ANSI_QUOTES;
    lex->unit.print(&str);
    thd->variables.sql_mode|= sql_mode;
  }
  str.append('\0');
  DBUG_PRINT("VIEW", ("View: %s", str.ptr()));

  /* print file name */
  (void) my_snprintf(dir_buff, FN_REFLEN, "%s/%s/",
		     mysql_data_home, view->db);
  unpack_filename(dir_buff, dir_buff);
  dir.str= dir_buff;
  dir.length= strlen(dir_buff);

  file.str= file_buff;
  file.length= (strxnmov(file_buff, FN_REFLEN, view->table_name, reg_ext,
                         NullS) - file_buff);
  /* init timestamp */
  if (!view->timestamp.str)
    view->timestamp.str= view->timestamp_buffer;

  /* check old .frm */
  {
    char path_buff[FN_REFLEN];
    LEX_STRING path;
    File_parser *parser;

    path.str= path_buff;
    fn_format(path_buff, file.str, dir.str, 0, MY_UNPACK_FILENAME);
    path.length= strlen(path_buff);

    if (!access(path.str, F_OK))
    {
      if (mode == VIEW_CREATE_NEW)
      {
	my_error(ER_TABLE_EXISTS_ERROR, MYF(0), view->alias);
	DBUG_RETURN(-1);
      }

      if (!(parser= sql_parse_prepare(&path, thd->mem_root, 0)))
	DBUG_RETURN(1);

      if (!parser->ok() ||
          strncmp("VIEW", parser->type()->str, parser->type()->length))
      {
        my_error(ER_WRONG_OBJECT, MYF(0),
                 (view->db ? view->db : thd->db), view->table_name, "VIEW");
        DBUG_RETURN(-1);
      }

      /*
        read revision number

        TODO: read dependence list, too, to process cascade/restrict
        TODO: special cascade/restrict procedure for alter?
      */
      if (parser->parse((gptr)view, thd->mem_root,
                        view_parameters + revision_number_position, 1))
      {
        DBUG_RETURN(thd->net.report_error? -1 : 0);
      }
    }
    else
    {
      if (mode == VIEW_ALTER)
      {
	my_error(ER_NO_SUCH_TABLE, MYF(0), view->db, view->alias);
	DBUG_RETURN(-1);
      }
    }
  }
  /* fill structure */
  view->query.str= (char*)str.ptr();
  view->query.length= str.length()-1; // we do not need last \0
  view->source.str= thd->query;
  view->source.length= thd->query_length;
  view->file_version= 1;
  view->calc_md5(md5);
  view->md5.str= md5;
  view->md5.length= 32;
  can_be_merged= lex->can_be_merged();
  if (lex->create_view_algorithm == VIEW_ALGORITHM_MERGE &&
      !lex->can_be_merged())
  {
    push_warning(thd, MYSQL_ERROR::WARN_LEVEL_WARN, ER_WARN_VIEW_MERGE,
                 ER(ER_WARN_VIEW_MERGE));
    lex->create_view_algorithm= VIEW_ALGORITHM_UNDEFINED;
  }
  view->algorithm= lex->create_view_algorithm;
  view->with_check= lex->create_view_check;
  if ((view->updatable_view= (can_be_merged &&
                              view->algorithm != VIEW_ALGORITHM_TMPTABLE)))
  {
    /* TODO: change here when we will support UNIONs */
    for (TABLE_LIST *tbl= (TABLE_LIST *)lex->select_lex.table_list.first;
	 tbl;
	 tbl= tbl->next_local)
    {
      if ((tbl->view && !tbl->updatable_view) || tbl->schema_table)
      {
	view->updatable_view= 0;
	break;
      }
      for (TABLE_LIST *up= tbl; up; up= up->embedding)
      {
	if (up->outer_join)
	{
	  view->updatable_view= 0;
	  goto loop_out;
	}
      }
    }
  }
loop_out:
  /*
    Check that table of main select do not used in subqueries.

    This test can catch only very simple cases of such non-updateable views,
    all other will be detected before updating commands execution.
    (it is more optimisation then real check)

    NOTE: this skip cases of using table via VIEWs, joined VIEWs, VIEWs with
    UNION
  */
  if (view->updatable_view &&
      !lex->select_lex.next_select() &&
      !((TABLE_LIST*)lex->select_lex.table_list.first)->next_local &&
      find_table_in_global_list(lex->query_tables->next_global,
				lex->query_tables->db,
				lex->query_tables->table_name))
  {
    view->updatable_view= 0;
  }

  if (view->with_check != VIEW_CHECK_NONE &&
      !view->updatable_view)
  {
    my_error(ER_VIEW_NONUPD_CHECK, MYF(0), view->db, view->table_name);
    DBUG_RETURN(-1);
  }

  if (sql_create_definition_file(&dir, &file, view_file_type,
				 (gptr)view, view_parameters, 3))
  {
    DBUG_RETURN(thd->net.report_error? -1 : 1);
  }
  DBUG_RETURN(0);
}


/*
  read VIEW .frm and create structures

  SYNOPSIS
    mysql_make_view()
    parser		- parser object;
    table		- TABLE_LIST structure for filling

  RETURN
    0 ok
    1 error
*/

my_bool
mysql_make_view(File_parser *parser, TABLE_LIST *table)
{
  DBUG_ENTER("mysql_make_view");
  DBUG_PRINT("info", ("table=%p (%s)", table, table->table_name));

  if (table->view)
  {
    DBUG_PRINT("info",
               ("VIEW %s.%s is already processed on previous PS/SP execution",
                table->view_db.str, table->view_name.str));
    DBUG_RETURN(0);
  }

  SELECT_LEX *end;
  THD *thd= current_thd;
  LEX *old_lex= thd->lex, *lex;
  SELECT_LEX *view_select;
  int res= 0;

  /*
    For now we assume that tables will not be changed during PS life (it
    will be TRUE as far as we make new table cache).
  */
  Item_arena *arena= thd->current_arena, backup;
  if (arena->is_conventional())
    arena= 0;
  else
    thd->set_n_backup_item_arena(arena, &backup);

  /* init timestamp */
  if (!table->timestamp.str)
    table->timestamp.str= table->timestamp_buffer;
  /*
    TODO: when VIEWs will be stored in cache, table mem_root should
    be used here
  */
  if (parser->parse((gptr)table, thd->mem_root, view_parameters,
                    required_view_parameters))
    goto err;

  /*
    Save VIEW parameters, which will be wiped out by derived table
    processing
  */
  table->view_db.str= table->db;
  table->view_db.length= table->db_length;
  table->view_name.str= table->table_name;
  table->view_name.length= table->table_name_length;

  /*TODO: md5 test here and warning if it is differ */

  /*
    TODO: TABLE mem root should be used here when VIEW will be stored in
    TABLE cache

    now Lex placed in statement memory
  */
  table->view= lex= thd->lex= (LEX*) new(thd->mem_root) st_lex_local;
  lex_start(thd, (uchar*)table->query.str, table->query.length);
  view_select= &lex->select_lex;
  view_select->select_number= ++thd->select_number;
  {
    ulong options= thd->options;
    /* switch off modes which can prevent normal parsing of VIEW
      - MODE_REAL_AS_FLOAT            affect only CREATE TABLE parsing
      + MODE_PIPES_AS_CONCAT          affect expression parsing
      + MODE_ANSI_QUOTES              affect expression parsing
      + MODE_IGNORE_SPACE             affect expression parsing
      - MODE_NOT_USED                 not used :)
      * MODE_ONLY_FULL_GROUP_BY       affect execution
      * MODE_NO_UNSIGNED_SUBTRACTION  affect execution
      - MODE_NO_DIR_IN_CREATE         affect table creation only
      - MODE_POSTGRESQL               compounded from other modes
      - MODE_ORACLE                   compounded from other modes
      - MODE_MSSQL                    compounded from other modes
      - MODE_DB2                      compounded from other modes
      - MODE_MAXDB                    affect only CREATE TABLE parsing
      - MODE_NO_KEY_OPTIONS           affect only SHOW
      - MODE_NO_TABLE_OPTIONS         affect only SHOW
      - MODE_NO_FIELD_OPTIONS         affect only SHOW
      - MODE_MYSQL323                 affect only SHOW
      - MODE_MYSQL40                  affect only SHOW
      - MODE_ANSI                     compounded from other modes
                                      (+ transaction mode)
      ? MODE_NO_AUTO_VALUE_ON_ZERO    affect UPDATEs
      + MODE_NO_BACKSLASH_ESCAPES     affect expression parsing
    */
    thd->options&= ~(MODE_PIPES_AS_CONCAT | MODE_ANSI_QUOTES |
                     MODE_IGNORE_SPACE | MODE_NO_BACKSLASH_ESCAPES);
    CHARSET_INFO *save_cs= thd->variables.character_set_client;
    thd->variables.character_set_client= system_charset_info;
    res= yyparse((void *)thd);
    thd->variables.character_set_client= save_cs;
    thd->options= options;
  }
  if (!res && !thd->is_fatal_error)
  {
    TABLE_LIST *top_view= (table->belong_to_view ?
                           table->belong_to_view :
                           table);
    TABLE_LIST *view_tables= lex->query_tables;
    TABLE_LIST *view_tables_tail= 0;
    TABLE_LIST *tbl;

    /*
      Check rights to run commands (EXPLAIN SELECT & SHOW CREATE) which show
      underlying tables.
      Skip this step if we are opening view for prelocking only.
    */
    if (!table->prelocking_placeholder &&
        (old_lex->sql_command == SQLCOM_SELECT && old_lex->describe))
    {
      if (check_table_access(thd, SELECT_ACL, view_tables, 1) &&
          check_table_access(thd, SHOW_VIEW_ACL, table, 1))
      {
        my_message(ER_VIEW_NO_EXPLAIN, ER(ER_VIEW_NO_EXPLAIN), MYF(0));
        goto err;
      }
    }
    else if (!table->prelocking_placeholder &&
             old_lex->sql_command == SQLCOM_SHOW_CREATE)
    {
      if (check_table_access(thd, SHOW_VIEW_ACL, table, 0))
        goto err;
    }

    /*
      mark to avoid temporary table using and put view reference and find
      last view table
    */
    for (tbl= view_tables;
         tbl;
         tbl= (view_tables_tail= tbl)->next_global)
    {
      tbl->skip_temporary= 1;
      tbl->belong_to_view= top_view;
    }

    /*
      Put tables of VIEW after VIEW TABLE_LIST

      NOTE: It is important for UPDATE/INSERT/DELETE checks to have this
      tables just after VIEW instead of tail of list, to be able check that
      table is unique. Also we store old next table for the same purpose.
    */
    if (view_tables)
    {
      if (table->next_global)
      {
        view_tables_tail->next_global= table->next_global;
        table->next_global->prev_global= &view_tables_tail->next_global;
      }
      else
      {
        old_lex->query_tables_last= &view_tables_tail->next_global;
      }
      view_tables->prev_global= &table->next_global;
      table->next_global= view_tables;
    }

    /*
      Let us set proper lock type for tables of the view's main select
      since we may want to perform update or insert on view. This won't
      work for view containing union. But this is ok since we don't
      allow insert and update on such views anyway.
    */
    if (!lex->select_lex.next_select())
      for (tbl= lex->select_lex.get_table_list(); tbl; tbl= tbl->next_local)
        tbl->lock_type= table->lock_type;

    /*
      If we are opening this view as part of implicit LOCK TABLES, then
      this view serves as simple placeholder and we should not continue
      further processing.
    */
    if (table->prelocking_placeholder)
      goto ok2;

    old_lex->derived_tables|= DERIVED_VIEW;

    /* move SQL_NO_CACHE & Co to whole query */
    old_lex->safe_to_cache_query= (old_lex->safe_to_cache_query &&
				   lex->safe_to_cache_query);
    /* move SQL_CACHE to whole query */
    if (view_select->options & OPTION_TO_QUERY_CACHE)
      old_lex->select_lex.options|= OPTION_TO_QUERY_CACHE;

    /*
      check MERGE algorithm ability
      - algorithm is not explicit TEMPORARY TABLE
      - VIEW SELECT allow merging
      - VIEW used in subquery or command support MERGE algorithm
    */
    if (table->algorithm != VIEW_ALGORITHM_TMPTABLE &&
	lex->can_be_merged() &&
        (table->select_lex->master_unit() != &old_lex->unit ||
         old_lex->can_use_merged()) &&
        !old_lex->can_not_use_merged())
    {
      /* lex should contain at least one table */
      DBUG_ASSERT(view_tables != 0);

      table->effective_algorithm= VIEW_ALGORITHM_MERGE;
      DBUG_PRINT("info", ("algorithm: MERGE"));
      table->updatable= (table->updatable_view != 0);
      table->effective_with_check= (uint8)table->with_check;

      table->ancestor= view_tables;

      /*
        Tables of the main select of the view should be marked as belonging
        to the same select as original view (again we can use LEX::select_lex
        for this purprose because we don't support MERGE algorithm for views
        with unions).
      */
      for (tbl= lex->select_lex.get_table_list(); tbl; tbl= tbl->next_local)
        tbl->select_lex= table->select_lex;

      {
        if (view_tables->next_local)
          table->multitable_view= TRUE;
        /* make nested join structure for view tables */
        NESTED_JOIN *nested_join;
        if (!(nested_join= table->nested_join=
              (NESTED_JOIN *) thd->calloc(sizeof(NESTED_JOIN))))
          goto err;
        nested_join->join_list= view_select->top_join_list;

        /* re-nest tables of VIEW */
        {
          List_iterator_fast<TABLE_LIST> ti(nested_join->join_list);
          while ((tbl= ti++))
          {
            tbl->join_list= &nested_join->join_list;
            tbl->embedding= table;
          }
        }
      }

      /* Store WHERE clause for post-processing in setup_ancestor */
      table->where= view_select->where;
      /*
        Add subqueries units to SELECT into which we merging current view.
        
        unit(->next)* chain starts with subqueries that are used by this
        view and continues with subqueries that are used by other views.
        We must not add any subquery twice (otherwise we'll form a loop),
        to do this we remember in end_unit the first subquery that has 
        been already added.
                
        NOTE: we do not support UNION here, so we take only one select
      */
      SELECT_LEX_NODE *end_unit= table->select_lex->slave;
      SELECT_LEX_UNIT *next_unit;
      for (SELECT_LEX_UNIT *unit= lex->select_lex.first_inner_unit();
           unit;
           unit= next_unit)
      {
        if (unit == end_unit)
          break;
        SELECT_LEX_NODE *save_slave= unit->slave;
        next_unit= unit->next_unit();
        unit->include_down(table->select_lex);
        unit->slave= save_slave; // fix include_down initialisation
      }

      /*
	This SELECT_LEX will be linked in global SELECT_LEX list
	to make it processed by mysql_handle_derived(),
	but it will not be included to SELECT_LEX tree, because it
	will not be executed
      */
      goto ok;
    }

    table->effective_algorithm= VIEW_ALGORITHM_TMPTABLE;
    DBUG_PRINT("info", ("algorithm: TEMPORARY TABLE"));
    view_select->linkage= DERIVED_TABLE_TYPE;
    table->updatable= 0;
    table->effective_with_check= VIEW_CHECK_NONE;
    old_lex->subqueries= TRUE;

    /* SELECT tree link */
    lex->unit.include_down(table->select_lex);
    lex->unit.slave= view_select; // fix include_down initialisation

    table->derived= &lex->unit;
  }
  else
    goto err;

ok:
  /* global SELECT list linking */
  end= view_select;	// primary SELECT_LEX is always last
  end->link_next= old_lex->all_selects_list;
  old_lex->all_selects_list->link_prev= &end->link_next;
  old_lex->all_selects_list= lex->all_selects_list;
  lex->all_selects_list->link_prev=
    (st_select_lex_node**)&old_lex->all_selects_list;

ok2:
  if (arena)
    thd->restore_backup_item_arena(arena, &backup);
  thd->lex= old_lex;
  DBUG_RETURN(0);

err:
  if (arena)
    thd->restore_backup_item_arena(arena, &backup);
  delete table->view;
  table->view= 0;	// now it is not VIEW placeholder
  thd->lex= old_lex;
  DBUG_RETURN(1);
}


/*
  drop view

  SYNOPSIS
    mysql_drop_view()
    thd		- thread handler
    views	- views to delete
    drop_mode	- cascade/check

  RETURN VALUE
    FALSE OK
    TRUE  Error
*/

bool mysql_drop_view(THD *thd, TABLE_LIST *views, enum_drop_mode drop_mode)
{
  DBUG_ENTER("mysql_drop_view");
  char path[FN_REFLEN];
  TABLE_LIST *view;
  bool type= 0;

  for (view= views; view; view= view->next_local)
  {
    strxnmov(path, FN_REFLEN, mysql_data_home, "/", view->db, "/",
             view->table_name, reg_ext, NullS);
    (void) unpack_filename(path, path);
    VOID(pthread_mutex_lock(&LOCK_open));
    if (access(path, F_OK) || (type= (mysql_frm_type(path) != FRMTYPE_VIEW)))
    {
      char name[FN_REFLEN];
      my_snprintf(name, sizeof(name), "%s.%s", view->db, view->table_name);
      if (thd->lex->drop_if_exists)
      {
	push_warning_printf(thd, MYSQL_ERROR::WARN_LEVEL_NOTE,
			    ER_BAD_TABLE_ERROR, ER(ER_BAD_TABLE_ERROR),
			    name);
	VOID(pthread_mutex_unlock(&LOCK_open));
	continue;
      }
      if (type)
        my_error(ER_WRONG_OBJECT, MYF(0), view->db, view->table_name, "VIEW");
      else
        my_error(ER_BAD_TABLE_ERROR, MYF(0), name);
      goto err;
    }
    if (my_delete(path, MYF(MY_WME)))
      goto err;
    query_cache_invalidate3(thd, view, 0);
    VOID(pthread_mutex_unlock(&LOCK_open));
  }
  send_ok(thd);
  DBUG_RETURN(FALSE);

err:
  VOID(pthread_mutex_unlock(&LOCK_open));
  DBUG_RETURN(TRUE);

}


/*
  Check type of .frm if we are not going to parse it

  SYNOPSIS
    mysql_frm_type()
    path	path to file

  RETURN
    FRMTYPE_ERROR	error
    FRMTYPE_TABLE	table
    FRMTYPE_VIEW	view
*/

frm_type_enum mysql_frm_type(char *path)
{
  File file;
  char header[10];	//"TYPE=VIEW\n" it is 10 characters
  int length;
  DBUG_ENTER("mysql_frm_type");

  if ((file= my_open(path, O_RDONLY | O_SHARE, MYF(MY_WME))) < 0)
  {
    DBUG_RETURN(FRMTYPE_ERROR);
  }
  length= my_read(file, (byte*) header, sizeof(header), MYF(MY_WME));
  my_close(file, MYF(MY_WME));
  if (length == (int) MY_FILE_ERROR)
    DBUG_RETURN(FRMTYPE_ERROR);
  if (length < (int) sizeof(header) ||
      !strncmp(header, "TYPE=VIEW\n", sizeof(header)))
    DBUG_RETURN(FRMTYPE_VIEW);
  DBUG_RETURN(FRMTYPE_TABLE);                   // Is probably a .frm table
}


/*
  check of key (primary or unique) presence in updatable view

  SYNOPSIS
    check_key_in_view()
    thd     thread handler
    view    view for check with opened table

  DESCRIPTION
    If it is VIEW and query have LIMIT clause then check that undertlying
    table of viey contain one of following:
      1) primary key of underlying table
      2) unique key underlying table with fields for which NULL value is
         impossible
      3) all fields of underlying table

  RETURN
    FALSE   OK
    TRUE    view do not contain key or all fields
*/

bool check_key_in_view(THD *thd, TABLE_LIST *view)
{
  TABLE *table;
  Field_translator *trans;
  KEY *key_info, *key_info_end;
  uint i, elements_in_view;
  DBUG_ENTER("check_key_in_view");

  /*
    we do not support updatable UNIONs in VIEW, so we can check just limit of
    LEX::select_lex
  */
  if ((!view->view && !view->belong_to_view) ||
      thd->lex->sql_command == SQLCOM_INSERT ||
      thd->lex->select_lex.select_limit == 0)
    DBUG_RETURN(FALSE); /* it is normal table or query without LIMIT */
  table= view->table;
  if (view->belong_to_view)
    view= view->belong_to_view;
  trans= view->field_translation;
  key_info_end= (key_info= table->key_info)+ table->s->keys;

  elements_in_view= view->view->select_lex.item_list.elements;
  DBUG_ASSERT(table != 0 && view->field_translation != 0);

  /* Loop over all keys to see if a unique-not-null key is used */
  for (;key_info != key_info_end ; key_info++)
  {
    if ((key_info->flags & (HA_NOSAME | HA_NULL_PART_KEY)) == HA_NOSAME)
    {
      KEY_PART_INFO *key_part= key_info->key_part;
      KEY_PART_INFO *key_part_end= key_part + key_info->key_parts;

      /* check that all key parts are used */
      for (;;)
      {
        uint k;
        for (k= 0; k < elements_in_view; k++)
        {
          Item_field *field;
          if ((field= trans[k].item->filed_for_view_update()) &&
              field->field == key_part->field)
            break;
        }
        if (k == elements_in_view)
          break;                                // Key is not possible
        if (++key_part == key_part_end)
          DBUG_RETURN(FALSE);                   // Found usable key
      }
    }
  }

  DBUG_PRINT("info", ("checking if all fields of table are used"));
  /* check all fields presence */
  {
    Field **field_ptr;
    for (field_ptr= table->field; *field_ptr; field_ptr++)
    {
      for (i= 0; i < elements_in_view; i++)
      {
        Item_field *field;
        if ((field= trans[i].item->filed_for_view_update()) &&
            field->field == *field_ptr)
          break;
      }
      if (i == elements_in_view)                // If field didn't exists
      {
        /*
          Keys or all fields of underlying tables are not foud => we have
          to check variable updatable_views_with_limit to decide should we
          issue an error or just a warning
        */
        if (thd->variables.updatable_views_with_limit)
        {
          /* update allowed, but issue warning */
          push_warning(thd, MYSQL_ERROR::WARN_LEVEL_NOTE,
                       ER_WARN_VIEW_WITHOUT_KEY, ER(ER_WARN_VIEW_WITHOUT_KEY));
          DBUG_RETURN(FALSE);
        }
        /* prohibit update */
        DBUG_RETURN(TRUE);
      }
    }
  }
  DBUG_RETURN(FALSE);
}


/*
  insert fields from VIEW (MERGE algorithm) into given list

  SYNOPSIS
    insert_view_fields()
    list      list for insertion
    view      view for processing

  RETURN
    FALSE OK
    TRUE  error (is not sent to cliet)
*/

bool insert_view_fields(List<Item> *list, TABLE_LIST *view)
{
  uint elements_in_view= view->view->select_lex.item_list.elements;
  Field_translator *trans;
  DBUG_ENTER("insert_view_fields");

  if (!(trans= view->field_translation))
    DBUG_RETURN(FALSE);

  for (uint i= 0; i < elements_in_view; i++)
  {
    Item_field *fld;
    if ((fld= trans[i].item->filed_for_view_update()))
      list->push_back(fld);
    else
    {
      my_error(ER_NON_UPDATABLE_TABLE, MYF(0), view->alias, "INSERT");
      DBUG_RETURN(TRUE);
    }
  }
  DBUG_RETURN(FALSE);
}

/*
  checking view md5 check suum

  SINOPSYS
    view_checksum()
    thd     threar handler
    view    view for check

  RETUIRN
    HA_ADMIN_OK               OK
    HA_ADMIN_NOT_IMPLEMENTED  it is not VIEW
    HA_ADMIN_WRONG_CHECKSUM   check sum is wrong
*/

int view_checksum(THD *thd, TABLE_LIST *view)
{
  char md5[MD5_BUFF_LENGTH];
  if (!view->view || view->md5.length != 32)
    return HA_ADMIN_NOT_IMPLEMENTED;
  view->calc_md5(md5);
  return (strncmp(md5, view->md5.str, 32) ?
          HA_ADMIN_WRONG_CHECKSUM :
          HA_ADMIN_OK);
}
