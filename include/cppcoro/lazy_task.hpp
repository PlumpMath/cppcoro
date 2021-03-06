///////////////////////////////////////////////////////////////////////////////
// Copyright (c) Lewis Baker
// Licenced under MIT license. See LICENSE.txt for details.
///////////////////////////////////////////////////////////////////////////////
#ifndef CPPCORO_LAZY_TASK_HPP_INCLUDED
#define CPPCORO_LAZY_TASK_HPP_INCLUDED

#include <cppcoro/broken_promise.hpp>

#include <atomic>
#include <exception>
#include <utility>
#include <type_traits>

#include <experimental/coroutine>

namespace cppcoro
{
	template<typename T> class lazy_task;

	namespace detail
	{
		class lazy_task_promise_base
		{
		public:

			lazy_task_promise_base() noexcept
				: m_awaiter(nullptr)
			{}

			auto initial_suspend() noexcept
			{
				return std::experimental::suspend_always{};
			}

			auto final_suspend() noexcept
			{
				struct awaitable
				{
					std::experimental::coroutine_handle<> m_awaiter;

					awaitable(std::experimental::coroutine_handle<> awaiter) noexcept
						: m_awaiter(awaiter)
					{}

					bool await_ready() const noexcept { return false; }

					void await_suspend(std::experimental::coroutine_handle<> coroutine)
					{
						m_awaiter.resume();
					}

					void await_resume() noexcept {}
				};

				return awaitable{ m_awaiter };
			}

			void unhandled_exception() noexcept
			{
				set_exception(std::current_exception());
			}

			void set_exception(std::exception_ptr exception)
			{
				m_exception = std::move(exception);
			}

			bool is_ready() const noexcept
			{
				return static_cast<bool>(m_awaiter);
			}

			void set_awaiter(std::experimental::coroutine_handle<> awaiter)
			{
				m_awaiter = awaiter;
			}

		protected:

			bool completed_with_unhandled_exception()
			{
				return m_exception != nullptr;
			}

			void rethrow_if_unhandled_exception()
			{
				if (m_exception != nullptr)
				{
					std::rethrow_exception(m_exception);
				}
			}

		private:

			std::experimental::coroutine_handle<> m_awaiter;
			std::exception_ptr m_exception;

		};

		template<typename T>
		class lazy_task_promise : public lazy_task_promise_base
		{
		public:

			lazy_task_promise() noexcept = default;

			~lazy_task_promise()
			{
				if (is_ready() && !completed_with_unhandled_exception())
				{
					reinterpret_cast<T*>(&m_valueStorage)->~T();
				}
			}

			auto get_return_object() noexcept
			{
				return std::experimental::coroutine_handle<lazy_task_promise>::from_promise(*this);
			}

			template<
				typename VALUE,
				typename = std::enable_if_t<std::is_convertible_v<VALUE&&, T>>>
			void return_value(VALUE&& value)
				noexcept(std::is_nothrow_constructible_v<T, VALUE&&>)
			{
				new (&m_valueStorage) T(std::forward<VALUE>(value));
			}

			T& result() &
			{
				rethrow_if_unhandled_exception();
				return *reinterpret_cast<T*>(&m_valueStorage);
			}

			T&& result() &&
			{
				rethrow_if_unhandled_exception();
				return std::move(*reinterpret_cast<T*>(&m_valueStorage));
			}

		private:

			// Not using std::aligned_storage here due to bug in MSVC 2015 Update 2
			// that means it doesn't work for types with alignof(T) > 8.
			// See MS-Connect bug #2658635.
			alignas(T) char m_valueStorage[sizeof(T)];

		};

		template<>
		class lazy_task_promise<void> : public lazy_task_promise_base
		{
		public:

			lazy_task_promise() noexcept = default;

			auto get_return_object() noexcept
			{
				return std::experimental::coroutine_handle<lazy_task_promise>::from_promise(*this);
			}

			void return_void() noexcept
			{}

			void result()
			{
				rethrow_if_unhandled_exception();
			}

		};

		template<typename T>
		class lazy_task_promise<T&> : public lazy_task_promise_base
		{
		public:

			lazy_task_promise() noexcept = default;

			auto get_return_object() noexcept
			{
				return std::experimental::coroutine_handle<lazy_task_promise>::from_promise(*this);
			}

			void return_value(T& value) noexcept
			{
				m_value = std::addressof(value);
			}

			T& result()
			{
				rethrow_if_unhandled_exception();
				return *m_value;
			}

		private:

			T* m_value;

		};
	}

	/// \brief
	/// A lazy task represents an asynchronous operation that is not started
	/// until it is first awaited.
	///
	/// When you call a coroutine that returns a lazy_task, the coroutine
	/// simply captures any passed parameters and returns exeuction to the
	/// caller. Execution of the coroutine body does not start until the
	/// coroutine is first co_await'ed.
	///
	/// Comparison with task<T>
	/// -----------------------
	/// The lazy task has lower overhead than cppcoro::task<T> as it does not
	/// require the use of atomic operations to synchronise potential races
	/// between the awaiting coroutine suspending and the coroutine completing.
	///
	/// The awaiting coroutine is suspended prior to the lazy_task being started
	/// which means that when the lazy_task completes it can unconditionally
	/// resume the awaiter.
	///
	/// One limitation of this approach is that if the lazy_task completes
	/// synchronously then, unless the compiler is able to perform tail-calls,
	/// the awaiting coroutine will be resumed inside a nested stack-frame.
	/// This call lead to stack-overflow if long chains of lazy_tasks complete
	/// synchronously.
	///
	/// The task<T> type does not have this issue as the awaiting coroutine is
	/// not suspended in the case that the task completes synchronously.
	template<typename T = void>
	class lazy_task
	{
	public:

		using promise_type = detail::lazy_task_promise<T>;

	private:

		struct awaitable_base
		{
			std::experimental::coroutine_handle<promise_type> m_coroutine;

			awaitable_base(std::experimental::coroutine_handle<promise_type> coroutine) noexcept
				: m_coroutine(coroutine)
			{}

			bool await_ready() const noexcept
			{
				return !m_coroutine || m_coroutine.promise().is_ready();
			}

			void await_suspend(std::experimental::coroutine_handle<> awaiter) noexcept
			{
				m_coroutine.promise().set_awaiter(awaiter);
				m_coroutine.resume();
			}
		};

	public:

		lazy_task() noexcept
			: m_coroutine(nullptr)
		{}

		explicit lazy_task(std::experimental::coroutine_handle<promise_type> coroutine)
			: m_coroutine(coroutine)
		{}

		lazy_task(lazy_task&& t) noexcept
			: m_coroutine(t.m_coroutine)
		{
			t.m_coroutine = nullptr;
		}

		/// Disable copy construction/assignment.
		lazy_task(const lazy_task&) = delete;
		lazy_task& operator=(const lazy_task&) = delete;

		/// Frees resources used by this task.
		~lazy_task()
		{
			if (m_coroutine)
			{
				m_coroutine.destroy();
			}
		}

		lazy_task& operator=(lazy_task&& other) noexcept
		{
			if (std::addressof(other) != this)
			{
				if (m_coroutine)
				{
					m_coroutine.destroy();
				}

				m_coroutine = other.m_coroutine;
				other.m_coroutine = nullptr;
			}

			return *this;
		}

		/// \brief
		/// Query if the task result is complete.
		///
		/// Awaiting a task that is ready will not block.
		bool is_ready() const noexcept
		{
			return !m_coroutine || m_coroutine.promise().is_ready();
		}

		auto operator co_await() const & noexcept
		{
			struct awaitable : awaitable_base
			{
				using awaitable_base::awaitable_base;

				decltype(auto) await_resume()
				{
					if (!m_coroutine)
					{
						throw broken_promise{};
					}

					return m_coroutine.promise().result();
				}
			};

			return awaitable{ m_coroutine };
		}

		auto operator co_await() const && noexcept
		{
			struct awaitable : awaitable_base
			{
				using awaitable_base::awaitable_base;

				decltype(auto) await_resume()
				{
					if (!m_coroutine)
					{
						throw broken_promise{};
					}

					return std::move(m_coroutine.promise()).result();
				}
			};

			return awaitable{ m_coroutine };
		}

		/// \brief
		/// Returns an awaitable that will await completion of the task without
		/// attempting to retrieve the result.
		auto when_ready() const noexcept
		{
			struct awaitable : awaitable_base
			{
				using awaitable_base::awaitable_base;

				void await_resume() const noexcept {}
			};

			return awaitable{ m_coroutine };
		}

	private:

		std::experimental::coroutine_handle<promise_type> m_coroutine;

	};
}

#endif
