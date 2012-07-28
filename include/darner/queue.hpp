#ifndef __DARNER_QUEUE_HPP__
#define __DARNER_QUEUE_HPP__

#include <stdexcept>
#include <set>

#include <boost/ptr_container/ptr_list.hpp>
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/function.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <leveldb/db.h>
#include <leveldb/iterator.h>
#include <leveldb/comparator.h>

namespace darner {

/*
 * queue is a fifo queue that is O(log(queue size / cache size)) for pushing/popping.  it boasts these features:
 *
 * - no blocking calls for the caller - blocking i/o can run on a separate i/o service thread
 * - an evented wait semantic for queue poppers
 * - items are first checked out, then later deleted or returned back into the queue
 *
 * queue will post events such as journal writes to a provided boost::asio io_service.  interrupting the
 * io_service with pending events is okay - queue is never in an inconsistent state between io events.
 *
 * queue is thread-safe, it synchronizes naively on a single asio strand
 */
class queue
{
public:

   typedef boost::uint64_t key_t;
   typedef boost::function<void (const boost::system::error_code& error)> push_callback;
   typedef boost::function<void (const boost::system::error_code& error, key_t key, const std::string& value)> pop_callback;
   typedef boost::function<void (const boost::system::error_code& error)> pop_end_callback;

   queue(boost::asio::io_service& ios,
         const std::string& journal_path)
   : journal_(NULL),
     cmp_(NULL),
     head_(0),
     tail_(0),
     strand_(ios)
   {
		leveldb::Options options;
   	options.create_if_missing = true;
      options.comparator = cmp_ = new comparator();
   	if (!leveldb::DB::Open(options, journal_path, &journal_).ok())
         throw std::runtime_error("can't open journal: " + journal_path);
   	leveldb::Iterator* it = journal_->NewIterator(leveldb::ReadOptions());
   	it->SeekToFirst();
   	if (it->Valid())
   		head_ = *reinterpret_cast<const key_t *>(it->key().data());
   	it->SeekToLast();
   	if (it->Valid())
   		tail_ = *reinterpret_cast<const key_t *>(it->key().data());
   }

   ~queue()
   {
      if (journal_)
         delete journal_;
      if (cmp_)
         delete cmp_;
   }

   /*
    * pushes a value into the queue.  calls cb after the write with a success code.  on failure, sets error as
    * io_error if there was a problem with the underlying journal
    */
   void push(const std::string& value, const push_callback& cb)
   {
   	strand_.post(boost::bind(&queue::push_, this, boost::cref(value), cb));
   }

   /*
    * reserves an item for popping off the back of the queue.  calls cb after at most wait_ms milliseconds with a
    * success code, the item's key, and the item's value.  on failure, sets error as either timed_out if no
    * items were available after wait_ms milliseconds, or io_error if there was a problem with the underlying journal
    */
   void pop(size_t wait_ms, const pop_callback& cb)
   {
      strand_.post(boost::bind(&queue::pop_, this, wait_ms, cb));
   }

   /*
    * finish the pop, either by deleting the item or returning back to the queue.  calls cb after the pop_end finishes
    * with a success code. on failure, sets error as io_error if there was a problem with the underlying journal.
    */
   void pop_end(key_t key, bool remove, const pop_end_callback& cb)
   {
   	strand_.post(boost::bind(&queue::pop_end_, this, key, remove, cb));         
   }

private:

	void push_(const std::string& value, const push_callback& cb)
	{
      leveldb::Slice skey(reinterpret_cast<const char *>(&tail_), sizeof(key_t));
   	if (!journal_->Put(leveldb::WriteOptions(), skey, value).ok())
   	{
         cb(boost::system::error_code(boost::system::errc::io_error, boost::system::system_category()));
   		return;
   	}

      ++tail_; // post-increment tail key in case leveldb write fails
   	cb(boost::system::error_code());

   	spin_waiters(); // in case there's a waiter waiting for this
	}

	void pop_(size_t wait_ms, const pop_callback& cb)
	{
      spin_waiters(); // first let's drive out any current waiters

      key_t key;
      if (!next_key(key)) // do we have an item right away?
      {
         if (wait_ms > 0) // okay, no item. can we fire up a timer and wait?
         {
            boost::ptr_list<waiter>::iterator it = waiters_.insert(waiters_.end(), new waiter(strand_, wait_ms, cb));
            it->timer.async_wait(strand_.wrap(
               boost::bind(&queue::waiter_timeout, this, boost::asio::placeholders::error, it)
            ));            
         }
         else
            cb(boost::asio::error::not_found, key_t(), ""); // nothing?  okay, return back no item
         return;
      }
      get_value(key, cb);
	}

   void pop_end_(key_t key, bool remove, const pop_end_callback& cb)
   {
      if (remove)
      {
         leveldb::Slice skey(reinterpret_cast<const char *>(&key), sizeof(key_t));
         if (!journal_->Delete(leveldb::WriteOptions(), skey).ok())
            cb(boost::system::error_code(boost::system::errc::io_error, boost::system::system_category()));
         else
            cb(boost::system::error_code());
      }
      else
      {
         returned_.insert(key);
         cb(boost::system::error_code());

         // in case there's a waiter waiting for this returned key, let's fire a check_pop_
         spin_waiters();
      }
   }

   // any operation that mutates the queue or the waiter state should run this to crank any pending events
	void spin_waiters()
	{
      while (true)
      {
         if (waiters_.empty())
            break;
         key_t key;
         if (!next_key(key))
            break;
         boost::ptr_list<waiter>::auto_type waiter = waiters_.release(waiters_.begin());
         waiter->timer.cancel();
         get_value(key, waiter->cb);
      }
	}

   // fetch the next key and return true if there is one
   bool next_key(key_t & key)
   {
      if (!returned_.empty())
      {
         key = *returned_.begin();
         returned_.erase(returned_.begin());
      }
      else if (head_ != tail_)
         key = head_++;
      else
         return false;
      return true;
   }

   // fetch the value in the journal at key and pass it to cb
   void get_value(key_t key, const pop_callback& cb)
   {
      leveldb::Slice skey(reinterpret_cast<const char *>(&key), sizeof(key_t));
      std::string value;
      boost::system::error_code e;
      if (!journal_->Get(leveldb::ReadOptions(), skey, &value).ok())
         e = boost::system::error_code(boost::system::errc::io_error, boost::system::system_category());
      cb(e, key, value);
   }

   // a waiter is a cheap struct that ties a callback to a deadline timer
   struct waiter
   {
      waiter(boost::asio::strand& strand_, size_t wait_ms, const pop_callback& _cb)
      : cb(_cb),
        timer(strand_.get_io_service(), boost::posix_time::milliseconds(wait_ms))
      {
      }

      pop_callback cb;
      boost::asio::deadline_timer timer;
   };

   void waiter_timeout(const boost::system::error_code& error, boost::ptr_list<waiter>::iterator waiter_it)
   {
      if (error == boost::asio::error::operation_aborted) // can be error if timer was canceled
         return;

      boost::ptr_list<waiter>::auto_type waiter = waiters_.release(waiter_it);

      if (error) // weird unspecified error, better pass it up just in case
         waiter->cb(error, key_t(), "");
      else
         waiter->cb(boost::asio::error::timed_out, key_t(), "");
   }

   // compare keys as native uint64's instead of lexically
   class comparator : public leveldb::Comparator
   {
   public:
      int Compare(const leveldb::Slice& a, const leveldb::Slice& b) const
      {
         boost::uint64_t uia = *reinterpret_cast<const boost::uint64_t*>(a.data());
         boost::uint64_t uib = *reinterpret_cast<const boost::uint64_t*>(b.data());
         if (uia < uib)
            return -1;
         if (uia > uib)
            return 1;
         return 0;
      }
      const char* Name() const { return "queue::comparator"; }
      void FindShortestSeparator(std::string*, const leveldb::Slice&) const { }
      void FindShortSuccessor(std::string*) const { }
   };

   leveldb::DB* journal_;
   comparator* cmp_;

   // layout of journal is:
   // --- < reserved or returned > --- | TAIL | --- < contiguous items > --- | HEAD |
   // items are pushed to head and popped from tail
   // reserved items are held by a connection and not finished yet
   // returned items were released by a connection but not deleted

   boost::uint64_t head_;
   boost::uint64_t tail_;
   std::set<key_t> returned_; // items < TAIL that were reserved but later returned (not popped)

   boost::ptr_list<waiter> waiters_;

   // all state mutation is synchronized on a single strand
   // no updates to the journal or to waiters can happen simultaenously
   boost::asio::strand strand_;
};

} // darner

#endif // __DARNER_QUEUE_HPP__