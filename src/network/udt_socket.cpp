#include <fc/network/udt_socket.hpp>
#include <fc/thread/thread.hpp>
#include <fc/thread/mutex.hpp>
#include <fc/thread/unique_lock.hpp>
#include <fc/network/ip.hpp>
#include <udt.h>

#include <arpa/inet.h>

namespace fc {
   
   class udt_epoll_service 
   {
      public:
         udt_epoll_service()
         :_epoll_thread("udt_epoll")
         {
            _epoll_id = UDT::epoll_create();
            _epoll_loop = _epoll_thread.async( [=](){ poll_loop(); } );
         }

         ~udt_epoll_service()
         {
            _epoll_loop.cancel();
         }

         void poll_loop()
         {
            while( !_epoll_loop.canceled() )
            {
               std::set<UDTSOCKET> read_ready;
               std::set<UDTSOCKET> write_ready;

               UDT::epoll_wait( _epoll_id, 
                                &read_ready, 
                                &write_ready, 1000 );

               { synchronized(_read_promises_mutex)
                  for( auto sock : read_ready )
                  {
                     auto itr = _read_promises.find( sock );
                     if( itr != _read_promises.end() )
                     {
                        itr->second->set_value();
                        _read_promises.erase(itr);
                     }
                  }
               } // synchronized read promise mutex

               { synchronized(_write_promises_mutex)
                  for( auto sock : write_ready )
                  {
                     auto itr = _write_promises.find( sock );
                     if( itr != _write_promises.end() )
                     {
                        itr->second->set_value();
                        _write_promises.erase(itr);
                     }
                  }
               } // synchronized write promise mutex
            } // while not canceled
         } // poll_loop

         void notify_read( int udt_socket_id, 
                           const promise<void>::ptr& p )
         {
            int events = UDT_EPOLL_IN;
            UDT::epoll_add_usock( _epoll_id, 
                                  udt_socket_id, 
                                  &events );

            { synchronized(_read_promises_mutex)


               _read_promises[udt_socket_id] = p;
            }
         }

         void notify_write( int udt_socket_id,
                            const promise<void>::ptr& p )
         {
            int events = UDT_EPOLL_OUT;
            UDT::epoll_add_usock( _epoll_id, 
                                  udt_socket_id, 
                                  &events );

            { synchronized(_write_promises_mutex)
               _write_promises[udt_socket_id] = p;
            }
         }

      private:
         fc::mutex                                    _read_promises_mutex;
         fc::mutex                                    _write_promises_mutex;
         std::unordered_map<int, promise<void>::ptr > _read_promises;
         std::unordered_map<int, promise<void>::ptr > _write_promises;

         fc::future<void> _epoll_loop;
         fc::thread _epoll_thread;
         int        _epoll_id;
   };



   void check_udt_errors()
   {
      UDT::ERRORINFO& error_info = UDT::getlasterror();
      if( error_info.getErrorCode() )
      {
         std::string  error_message = error_info.getErrorMessage();
         error_info.clear();
         FC_CAPTURE_AND_THROW( udt_exception, (error_message) );
      }
   }

   udt_socket::udt_socket()
   :_udt_socket_id( UDT::INVALID_SOCK )
   {
   }

   udt_socket::~udt_socket()
   {
      close();
   }

   void udt_socket::connect_to( const ip::endpoint& remote_endpoint )
   { try {
      sockaddr_in serv_addr;
      serv_addr.sin_family = AF_INET;
      serv_addr.sin_port = htons(remote_endpoint.port());
      serv_addr.sin_addr.s_addr = htonl(remote_endpoint.get_address());

      // connect to the server, implict bind
      if (UDT::ERROR == UDT::connect(_udt_socket_id, (sockaddr*)&serv_addr, sizeof(serv_addr)))
         check_udt_errors();

   } FC_CAPTURE_AND_RETHROW( (remote_endpoint) ) }

   ip::endpoint udt_socket::remote_endpoint() const
   { try {
      sockaddr_in peer_addr;
      int peer_addr_size = sizeof(peer_addr);
      int error_code = UDT::getpeername( _udt_socket_id, (struct sockaddr*)&peer_addr, &peer_addr_size );
      if( error_code == UDT::ERROR )
          check_udt_errors();
      return ip::endpoint( ip::address( htonl( peer_addr.sin_addr.s_addr ) ), htons(peer_addr.sin_port) );
   } FC_CAPTURE_AND_RETHROW() }

   ip::endpoint udt_socket::local_endpoint() const
   { try {
      sockaddr_in sock_addr;
      int addr_size = sizeof(sock_addr);
      int error_code = UDT::getsockname( _udt_socket_id, (struct sockaddr*)&sock_addr, &addr_size );
      if( error_code == UDT::ERROR )
          check_udt_errors();
      return ip::endpoint( ip::address( htonl( sock_addr.sin_addr.s_addr ) ), htons(sock_addr.sin_port) );
   } FC_CAPTURE_AND_RETHROW() }


   /// @{
   size_t   udt_socket::readsome( char* buffer, size_t max )
   { try {
      auto bytes_read = UDT::recv( _udt_socket_id, buffer, max, 0 );
      if( bytes_read == UDT::ERROR )
      {
         if( UDT::getlasterror().getErrorCode() == CUDTException::EASYNCRCV )
         {
            // create a future and post to epoll, wait on it, then
            // call readsome recursively.
         }
         else
            check_udt_errors();
      }
      return bytes_read;
   } FC_CAPTURE_AND_RETHROW( (max) ) }

   bool     udt_socket::eof()const
   {
      // TODO... 
      return false;
   }
   /// @}
   
   /// ostream interface
   /// @{
   size_t   udt_socket::writesome( const char* buffer, size_t len )
   {
      auto bytes_sent = UDT::send(_udt_socket_id, buffer, len, 0);

      if( UDT::ERROR == bytes_sent )
         check_udt_errors();

      if( bytes_sent == 0 )
      {
         // schedule wait with epoll 
      }
      return bytes_sent;
   }

   void     udt_socket::flush(){}

   void     udt_socket::close()
   { try {
      UDT::close( _udt_socket_id );
      check_udt_errors();
   } FC_CAPTURE_AND_RETHROW() }
   /// @}
   
   void udt_socket::open()
   {
      _udt_socket_id = UDT::socket(AF_INET, SOCK_STREAM, 0);
   }

   bool udt_socket::is_open()const
   {
      return _udt_socket_id != UDT::INVALID_SOCK;
   }
     

} 
