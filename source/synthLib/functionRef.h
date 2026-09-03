#pragma once

#include <memory>
#include <type_traits>
#include <utility>

namespace synthLib
{
	template<typename Signature>
	class FunctionRef;

	// A non-owning reference to a callable that is invoked synchronously. Unlike
	// std::function this never copies the callable or allocates storage for it.
	template<typename Result, typename... Args>
	class FunctionRef<Result(Args...)>
	{
	public:
		template<typename Callable,
			typename = std::enable_if_t<!std::is_same_v<
				std::decay_t<Callable>, FunctionRef>>>
		FunctionRef(Callable&& _callable) noexcept
			: m_object(const_cast<void*>(static_cast<const void*>(
				std::addressof(_callable))))
			, m_callback([](void* const _object, Args... _args) -> Result
			{
				return (*static_cast<std::remove_reference_t<Callable>*>(_object))(
					std::forward<Args>(_args)...);
			})
		{
		}

		Result operator()(Args... _args) const
		{
			return m_callback(m_object, std::forward<Args>(_args)...);
		}

	private:
		void* m_object;
		Result (*m_callback)(void*, Args...);
	};
}
