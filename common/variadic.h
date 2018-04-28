#ifndef __VARIADIC_H_
#define __VARIADIC_H_

/*
 * A few utility things to help with variadic template packs.
 * We do not want to require C++14 yet, so we need to make some trivial things
 * like std::make_index_sequence by ourselves.
 */

#include <type_traits>

namespace nbl {

template<size_t...>
struct index_sequence {};

namespace detail
{
	// make_index_sequence
	template<size_t N, size_t... S>
	struct make_index_sequence_impl
		: make_index_sequence_impl<N-1, N-1, S...>
	{};
	template<size_t... S>
	struct make_index_sequence_impl<0, S...>
	{
		using type = index_sequence<S...>;
	};


	// type_at_index
	template<size_t N, typename T0, typename... Trest>
	struct type_at_index_impl
		: type_at_index_impl<N-1, Trest...>
	{};
	template<typename T0, typename... Trest>
	struct type_at_index_impl<0, T0, Trest...>
	{
		using type = T0;
	};


	// Repeat type T multiple times in the template list of type TT
	template<typename T, template<typename...> class TT, typename sequence>
	struct repeat;
	template<typename T, template<typename...> class TT, size_t... s>
	struct repeat<T, TT, index_sequence<s...>>
	{
		template<typename a, size_t>
		using dep = a;

		using type = TT<dep<T, s>...>;
	};


	// Calling constructor for values in tuple
	template<typename class_t, typename tuple_t, size_t... s>
	inline constexpr class_t make_from_tuple(tuple_t&& t, index_sequence<s...>)
	{
		return class_t( std::get<s>(std::forward<tuple_t>(t))... );
	}
} // namespace detail


// Analogous to std::make_index_sequence
template<size_t N>
using make_index_sequence = typename detail::make_index_sequence_impl<N>::type;


// Get type at given index in parameter pack
template<size_t I, typename... types>
using type_at_index = typename detail::type_at_index_impl<I, types...>::type;


// Repeat type T N times in the template list of type TT
template<typename T, size_t N, template<typename...> class TT>
using repeat = typename detail::repeat<T, TT, make_index_sequence<N>>::type;


// Analogous to std::make_from_tuple.
// Call constructor, unpacking a std::tuple (or std::array) for parameters.
template<typename class_t, typename tuple_t>
inline constexpr class_t make_from_tuple(tuple_t&& t)
{
	return detail::make_from_tuple<class_t>(std::forward<tuple_t>(t),
		make_index_sequence<
			std::tuple_size<typename std::remove_reference<tuple_t>::type>::value
		>());
}

} // namespace nbl

#endif // __VARIADIC_H_
