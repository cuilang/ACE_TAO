// Proactor.cpp
// $Id: Proactor.cpp,v

#define ACE_BUILD_DLL
#include "ace/Proactor.h"

#if defined (ACE_WIN32)
// This only works on Win32 platforms

#include "ace/Task.h"
#include "ace/Log_Msg.h"

#if !defined (__ACE_INLINE__)
#include "ace/Proactor.i"
#endif /* __ACE_INLINE__ */

class ACE_Export ACE_Proactor_Timer_Handler : public ACE_Task <ACE_NULL_SYNCH>
  //     
  // = TITLE
  //
  //     A Handler for timer. It helps in the management of timers
  //     registered with the Proactor. 
  //
  // = DESCRIPTION 
  //     
  //     This object has a thread that will wait on the earliest
  //     time in a list of timers and an event. When a timer
  //     expires, the thread will post a completion event on the
  //     port and go back to waiting on the timer queue and
  //     event. If the event is signaled, the thread will refresh
  //     the time it is currently waiting on (in case the earliest
  //     time has changed)
  // 
{
  friend class ACE_Proactor;
  // Proactor has special privileges
  // Access needed to: timer_event_
  
public:
  ACE_Proactor_Timer_Handler (ACE_Proactor &proactor);
  
protected:
  virtual int svc (void);
  // Run by a daemon thread to handle deferred processing. In other
  // words, this method will do the waiting on the earliest timer
  // and event
  
  ACE_Auto_Event timer_event_;
  // Event to wait on
  
  ACE_Proactor &proactor_;
  // Proactor 
};
  
ACE_Proactor_Timer_Handler::ACE_Proactor_Timer_Handler (ACE_Proactor &proactor)
  : proactor_ (proactor),
    ACE_Task <ACE_NULL_SYNCH> (&proactor.thr_mgr_)
{
}

int
ACE_Proactor_Timer_Handler::svc (void)
{
  u_long time;
  ACE_Time_Value absolute_time;

  for (;;)
    {
      // default value
      time = INFINITE;

      // If the timer queue is not empty
      if (!this->proactor_.timer_queue ()->is_empty ())
	{
	  // Get the earliest absolute time
	  absolute_time 
	    = this->proactor_.timer_queue ()->earliest_time () 
	    - this->proactor_.timer_queue ()->gettimeofday ();
	  
	  // time to wait
	  time = absolute_time.msec ();
	  
	  // Make sure the time is positive
	  if (time < 0)
	    time = 0;
	}
      
      // Wait for event upto <time> milli seconds
      int result = ::WaitForSingleObject (this->timer_event_.handle (),
					  time);
      switch (result)
	{
	case WAIT_TIMEOUT:
	  // timeout: expire timers
	  this->proactor_.timer_queue ()->expire ();
	  break;
	case WAIT_FAILED:
	  // error 
	  ACE_ERROR_RETURN ((LM_ERROR, "%p\n", "WaitForSingleObject"), -1);
	}
    }

  return 0;  
}

ACE_Proactor_Handle_Timeout_Upcall::ACE_Proactor_Handle_Timeout_Upcall (void)
  : proactor_ (0)
{
}

int
ACE_Proactor_Handle_Timeout_Upcall::operator () (TIMER_QUEUE &timer_queue,
						 ACE_Handler *handler,
						 const void *act,
						 const ACE_Time_Value &time)
{
  ACE_UNUSED_ARG (timer_queue);

  if (this->proactor_ == 0)
    ACE_ERROR_RETURN ((LM_ERROR, 
		       "(%t) No Proactor set in ACE_Proactor_Handle_Timeout_Upcall, no completion port to post timeout to?!@\n"),
		      -1);
  
  // Create the Asynch_Timer
  ACE_Proactor::Asynch_Timer *asynch_timer 
    = new ACE_Proactor::Asynch_Timer (*handler,
				      act,
				      time);
  
  // Post a completion
  if (::PostQueuedCompletionStatus (this->proactor_->completion_port_, // completion port
				    0, // number of bytes tranferred 
				    0,	// completion key 
				    asynch_timer // overlapped
				    ) == FALSE)
    {
      delete asynch_timer;
      ACE_ERROR_RETURN ((LM_ERROR, "Failure in dealing with timers: PostQueuedCompletionStatus failed\n"), -1);
    }
  
  return 0;
}


int
ACE_Proactor_Handle_Timeout_Upcall::operator () (TIMER_QUEUE &timer_queue,
						 ACE_Handler *handler)
{
  ACE_UNUSED_ARG (timer_queue);
  ACE_UNUSED_ARG (handler);

  // Do nothing
  return 0;
}

int
ACE_Proactor_Handle_Timeout_Upcall::proactor (ACE_Proactor &proactor)
{
  if (this->proactor_ == 0)
    {
      this->proactor_ = &proactor;
      return 0;
    }
  else
    ACE_ERROR_RETURN ((LM_ERROR, 
		       "ACE_Proactor_Handle_Timeout_Upcall is only suppose to be used with ONE (and only one) Proactor\n"), 
		      -1);
}



ACE_Proactor::ACE_Proactor (size_t number_of_threads, 
			    Timer_Queue *tq)
  : completion_port_ (0), // This *MUST* be 0, *NOT* ACE_INVALID_HANDLE!!!!
    number_of_threads_ (number_of_threads),
    timer_queue_ (0),
    delete_timer_queue_ (0),
    timer_handler_ (0)
{
  // create the completion port
  this->completion_port_ = ::CreateIoCompletionPort (INVALID_HANDLE_VALUE,						     
						     this->completion_port_,
						     0,
						     this->number_of_threads_);
  if (this->completion_port_ == 0)
    ACE_ERROR ((LM_ERROR, "%p\n", "CreateIoCompletionPort"));

  // set the timer queue
  this->timer_queue (tq);

  // Create the timer handler
  ACE_NEW (this->timer_handler_, ACE_Proactor_Timer_Handler (*this));

  // activate <timer_handler>
  if (this->timer_handler_->activate () == -1)
    ACE_ERROR ((LM_ERROR, "%p Could not create thread\n", "Task::activate"));

}

ACE_Proactor::~ACE_Proactor (void)
{
  this->close ();
}

int 
ACE_Proactor::close (void)
{
  // Take care of the timer handler
  if (this->timer_handler_)
    {
      delete this->timer_handler_;
      this->timer_handler_ = 0;
    }

  // Take care of the timer queue
  if (this->delete_timer_queue_)
    {
      delete this->timer_queue_;
      this->timer_queue_ = 0;
      this->delete_timer_queue_ = 0;
    }

  // Close the completion port
  if (this->completion_port_ != 0)
    {
      int result = ACE_OS::close (this->completion_port_);
      this->completion_port_ = 0;
      return result;
    }
  else
    return 0;
}

int 
ACE_Proactor::register_handle (ACE_HANDLE handle, 
			       const void *completion_key)
{
  // No locking is needed here as no state changes
  ACE_HANDLE cp = ::CreateIoCompletionPort (handle,
					    this->completion_port_,
					    (u_long) completion_key,
					    this->number_of_threads_);
  if (cp == 0)
    {
      errno = ::GetLastError ();
      // If errno == ERROR_INVALID_PARAMETER, then this handle was
      // already registered.
      if (errno != ERROR_INVALID_PARAMETER)
	ACE_ERROR_RETURN ((LM_ERROR, "%p\n", "CreateIoCompletionPort"), -1);
    }
  return 0;
}

int 
ACE_Proactor::schedule_timer (ACE_Handler &handler, 
			      const void *act,
			      const ACE_Time_Value &time)
{
  return this->schedule_timer (handler, act, time, ACE_Time_Value::zero);
}

int 
ACE_Proactor::schedule_repeating_timer (ACE_Handler &handler, 
					const void *act,
					const ACE_Time_Value &interval)
{
  return this->schedule_timer (handler, act, interval, interval);
}

int 
ACE_Proactor::schedule_timer (ACE_Handler &handler, 
			      const void *act,
			      const ACE_Time_Value &time,
			      const ACE_Time_Value &interval)
{
  // absolute time
  ACE_Time_Value absolute_time = this->timer_queue_->gettimeofday () + time;
  
  // Only one guy goes in here at a time
  ACE_GUARD_RETURN (ACE_Recursive_Thread_Mutex, ace_mon, this->timer_queue_->lock (), -1);  

  // Schedule the timer
  int result = this->timer_queue_->schedule (&handler, 
					     act,
					     absolute_time,
					     interval);
  if (result != -1)
    {
      // no failures: check to see if we are the earliest time
      if (this->timer_queue_->earliest_time () == absolute_time)
	
	// wake up the timer thread
	if (this->timer_handler_->timer_event_.signal () == -1)
	  {
	    // Cancel timer
	    this->timer_queue_->cancel (result);
	    result = -1;
	  }
    }
  return result;
}

int 
ACE_Proactor::cancel_timer (int timer_id, 
			    const void **arg)
{
  // No need to singal timer event here. Even if the cancel timer was
  // the earliest, we will have an extra wakeup.
  return this->timer_queue_->cancel (timer_id, arg);
}

int 
ACE_Proactor::cancel_timer (ACE_Handler &handler)
{
  // No need to singal timer event here. Even if the cancel timer was
  // the earliest, we will have an extra wakeup.
  return this->timer_queue_->cancel (&handler);
}

int 
ACE_Proactor::handle_events (ACE_Time_Value &wait_time)
{
  // Decrement <wait_time> with the amount of time spent in the method
  ACE_Countdown_Time countdown (&wait_time);
  return this->handle_events (wait_time.msec ());
}

int 
ACE_Proactor::handle_events (void)
{
  return this->handle_events (INFINITE);
}

int 
ACE_Proactor::handle_events (unsigned long milli_seconds)
{
  OVERLAPPED *overlapped = 0;
  u_long bytes_transferred = 0;
  u_long completion_key = 0;

  // Get the next asynchronous operation that completes
  BOOL result = ::GetQueuedCompletionStatus (this->completion_port_,
					     &bytes_transferred,
					     &completion_key,
					     &overlapped,
					     milli_seconds);

  if (result == FALSE && overlapped == 0)
    {
      errno = ::GetLastError ();
      // @@  What's the WIN32 constant for timeout (258)?!?!?!
      if (errno == 258)
	{
	  errno = ETIMEDOUT;
	  return 0;
	}
      else
	ACE_ERROR_RETURN ((LM_ERROR, "%p\n", "GetQueuedCompletionStatus"), -1);
    }
  else
    {
      // Narrow result
      ACE_Asynch_Result *asynch_result = (ACE_Asynch_Result *) overlapped;
      // If errors happen, grab the error
      if (result == FALSE)
	errno = ::GetLastError ();
      
      this->application_specific_code (asynch_result,
				       bytes_transferred,
				       result,
				       (void *) completion_key,
				       errno);
    }
  return 0;
}

void
ACE_Proactor::application_specific_code (ACE_Asynch_Result *asynch_result,
					 u_long bytes_transferred,
					 int success,
					 const void *completion_key,
					 u_long error)
{
  ACE_SEH_TRY
    {
      // Call completion hook
      asynch_result->complete (bytes_transferred,
			       success,
			       (void *) completion_key,
			       errno);
    }
  ACE_SEH_FINALLY
    {
      // This is crucial to prevent memory leaks
      delete asynch_result;
    }
}

int 
ACE_Proactor::run_proactor_event_loop (void)
{
  return 0;
}

int 
ACE_Proactor::run_event_loop (ACE_Time_Value &)
{
  return 0;
}

int 
ACE_Proactor::end_event_loop (void)
{
  return 0;
}

sig_atomic_t 
ACE_Proactor::event_loop_done (void)
{
  return 0;
}

int 
ACE_Proactor::wake_up_dispatch_threads (void)
{
  return 0;
}

int 
ACE_Proactor::close_dispatch_threads (int)
{
  return 0;
}

size_t 
ACE_Proactor::number_of_threads (void) const
{
  return this->number_of_threads_;
}

void 
ACE_Proactor::number_of_threads (size_t threads)
{
  this->number_of_threads_ = threads;
}

ACE_Proactor::Timer_Queue *
ACE_Proactor::timer_queue (void) const
{
  return this->timer_queue_;
}

void 
ACE_Proactor::timer_queue (Timer_Queue *tq)
{
  // cleanup old timer queue
  if (this->delete_timer_queue_)
    {
      delete this->timer_queue_;
      this->delete_timer_queue_ = 0;
    }

  // new timer queue
  if (tq == 0)
    {
      this->timer_queue_ = new Timer_List;
      this->delete_timer_queue_ = 1;
    }
  else
    {
      this->timer_queue_ = tq;
      this->delete_timer_queue_ = 0;
    }

  // Set the proactor in the timer queue's functor
  this->timer_queue_->upcall_functor ().proactor (*this);
}

ACE_Proactor::Asynch_Timer::Asynch_Timer (ACE_Handler &handler,
					  const void *act,
					  const ACE_Time_Value &tv)
  : ACE_Asynch_Result (handler,
		       act),
    time_ (tv)
{
}

void
ACE_Proactor::Asynch_Timer::complete (u_long bytes_transferred,
				      int success,
				      const void *completion_key,
				      u_long error)
{
  this->handler_.handle_timeout (this->time_, this->act ());
}

#if defined (ACE_TEMPLATES_REQUIRE_SPECIALIZATION)

template class ACE_Unbounded_Set<ACE_Timer_Node_T<ACE_Handler *, ACE_Proactor_Handle_Timeout_Upcall> *>;
template class ACE_Unbounded_Set_Iterator<ACE_Timer_Node_T<ACE_Handler *, ACE_Proactor_Handle_Timeout_Upcall> *>;
template class ACE_Set_Node<ACE_Timer_Node_T<ACE_Handler *, ACE_Proactor_Handle_Timeout_Upcall> *>;

template class ACE_Timer_Node_T<ACE_Handler *, ACE_Proactor_Handle_Timeout_Upcall>;

template class ACE_Timer_Queue_T<ACE_Handler *, ACE_Proactor_Handle_Timeout_Upcall>;
template class ACE_Timer_Queue_Iterator_T<ACE_Handler *, ACE_Proactor_Handle_Timeout_Upcall>;

template class ACE_Timer_List_T<ACE_Handler *, ACE_Proactor_Handle_Timeout_Upcall>;
template class ACE_Timer_List_Iterator_T<ACE_Handler *, ACE_Proactor_Handle_Timeout_Upcall>;

template class ACE_Timer_Heap_T<ACE_Handler *, ACE_Proactor_Handle_Timeout_Upcall>;
template class ACE_Timer_Heap_Iterator_T<ACE_Handler *, ACE_Proactor_Handle_Timeout_Upcall>;

template class ACE_Timer_Wheel_T<ACE_Handler *, ACE_Proactor_Handle_Timeout_Upcall>;
template class ACE_Timer_Wheel_Iterator_T<ACE_Handler *, ACE_Proactor_Handle_Timeout_Upcall>;

#endif /* ACE_TEMPLATES_REQUIRE_SPECIALIZATION */

#endif /* ACE_WIN32 */
