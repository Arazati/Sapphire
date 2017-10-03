#include "DbConnection.h"
#include "DbWorker.h"
#include "src/libraries/sapphire/mysqlConnector/MySqlConnector.h"

#include "src/servers/Server_Common/Logging/Logger.h"
#include "PreparedStatement.h"


extern Core::Logger g_log;

Core::Db::DbConnection::DbConnection( ConnectionInfo &connInfo ) :
   m_reconnecting( false ),
   m_prepareError( false ),
   m_queue( nullptr ),
   m_pConnection( nullptr ),
   m_connectionInfo( connInfo ),
   m_connectionFlags( CONNECTION_SYNCH )
{

}

Core::Db::DbConnection::DbConnection( Core::LockedWaitQueue<Operation *>* queue, Core::Db::ConnectionInfo& connInfo ) :
   m_reconnecting( false ),
   m_prepareError( false ),
   m_queue( queue ),
   m_pConnection( nullptr ),
   m_connectionInfo( connInfo ),
   m_connectionFlags( CONNECTION_ASYNC )
{
   m_worker = std::unique_ptr< DbWorker >( new DbWorker( m_queue, this ) );
}

Core::Db::DbConnection::~DbConnection()
{
   close();
}

void Core::Db::DbConnection::close()
{
   m_worker.reset();
   m_stmts.clear();

   if( m_pConnection )
   {
      m_pConnection->close();
      m_pConnection.reset();
   }


}

uint32_t Core::Db::DbConnection::open()
{
   Mysql::MySqlBase base;
   Mysql::optionMap options;
   options[ MYSQL_OPT_RECONNECT ] = "1";
   options[ MYSQL_SET_CHARSET_NAME ] = "utf8";

   try
   {
      m_pConnection = std::shared_ptr< Mysql::Connection >( base.connect( m_connectionInfo.host,
                                                                          m_connectionInfo.user,
                                                                          m_connectionInfo.password,
                                                                          options,
                                                                          m_connectionInfo.port ) );
      m_pConnection->setSchema( m_connectionInfo.database );
   }
   catch( std::runtime_error& e )
   {
      g_log.error( e.what() );
      return 1;
   }

   return 0;
}

uint32_t Core::Db::DbConnection::getLastError()
{
   return m_pConnection->getErrorNo();
}

void Core::Db::DbConnection::ping()
{
   m_pConnection->ping();
}

bool Core::Db::DbConnection::lockIfReady()
{
   return m_mutex.try_lock();
}

void Core::Db::DbConnection::unlock()
{
   m_mutex.unlock();
}

void Core::Db::DbConnection::beginTransaction()
{
   m_pConnection->beginTransaction();
}

void Core::Db::DbConnection::rollbackTransaction()
{
   m_pConnection->rollbackTransaction();
}

void Core::Db::DbConnection::commitTransaction()
{
   m_pConnection->commitTransaction();
}

bool Core::Db::DbConnection::execute( const std::string& sql )
{
   try
   {
      Mysql::Statement* stmt( m_pConnection->createStatement() );
      bool result = stmt->execute( sql );
      return result;
   }
   catch( std::runtime_error& e )
   {
      g_log.error( e.what() );
      return false;
   }
}

Mysql::ResultSet *Core::Db::DbConnection::query( const std::string& sql )
{
   try
   {
      Mysql::Statement* stmt( m_pConnection->createStatement() );
      Mysql::ResultSet* result = stmt->executeQuery( sql );
      return result;
   }
   catch( std::runtime_error& e )
   {
      g_log.error( e.what() );
      return nullptr;
   }
}


Mysql::ResultSet* Core::Db::DbConnection::query( Core::Db::PreparedStatement* stmt )
{
   Mysql::ResultSet* res = nullptr;
   if( !stmt )
      return nullptr;

   uint32_t index = stmt->getIndex();

   Mysql::PreparedStatement* pStmt = getPreparedStatement( index );

   if( !pStmt )
      return nullptr;

   stmt->setMysqlPS( pStmt );
   try
   {
      stmt->bindParameters();
      return pStmt->executeQuery();
   }
   catch( std::runtime_error& e )
   {
      g_log.error( e.what() );
      return nullptr;
   }
   
}

bool Core::Db::DbConnection::execute( Core::Db::PreparedStatement* stmt )
{
   if( !stmt )
      return false;

   uint32_t index = stmt->getIndex();

   Mysql::PreparedStatement* pStmt = getPreparedStatement( index );

   if( !pStmt )
      return false;

   stmt->setMysqlPS( pStmt );
   try
   {
      stmt->bindParameters();
      return pStmt->execute();
   }
   catch( std::runtime_error& e )
   {
      g_log.error( e.what() );
      return nullptr;
   }
}

Mysql::PreparedStatement* Core::Db::DbConnection::getPreparedStatement( uint32_t index )
{
   assert( index < m_stmts.size() );
   Mysql::PreparedStatement* ret = m_stmts[index].get();
   if( !ret )
     nullptr;

   return ret;
}

void Core::Db::DbConnection::prepareStatement( uint32_t index, const std::string &sql, Core::Db::ConnectionFlags flags )
{
   m_queries.insert( PreparedStatementMap::value_type( index, std::make_pair( sql, flags ) ) );

   // Check if specified query should be prepared on this connection
   // i.e. don't prepare async statements on synchronous connections
   // to save memory that will not be used.
   if( !( m_connectionFlags & flags ) )
   {
      m_stmts[index].reset();
      return;
   }

   Mysql::PreparedStatement* pStmt = nullptr;

   try
   {
      pStmt = m_pConnection->prepareStatement( sql );
   }
   catch( std::runtime_error& e )
   {
      g_log.error( e.what() );
      m_prepareError = true;
   }

   m_stmts[index] = std::unique_ptr< Mysql::PreparedStatement >( pStmt );

}

bool Core::Db::DbConnection::prepareStatements()
{
   doPrepareStatements();
   return !m_prepareError;
}





