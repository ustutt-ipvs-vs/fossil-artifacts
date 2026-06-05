#pragma once

#include <functional>
#include <tuple>
#include <type_traits>

namespace fossil::detail {
namespace func_traits {

// function pointers
template<typename Ret, typename... Args>
auto get_args(Ret (*)(Args...)) -> std::tuple<Args...>;

// function pointers
template<typename Ret>
auto get_args(Ret (*)()) -> void;

// functor/lambda
template<typename F, typename Ret, typename... Args>
auto get_args(Ret (F::*)(Args...)) -> std::tuple<Args...>;

// functor/lambda
template<typename F, typename Ret, typename... Args>
auto get_args(Ret (F::*)(Args...) const) -> std::tuple<Args...>;

// functor/lambda
template<typename F, typename Ret>
auto get_args(Ret (F::*)()) -> void;

// functor/lambda
template<typename F, typename Ret>
auto get_args(Ret (F::*)() const) -> void;


template<class F>
requires std::is_class_v<F>
auto args() -> decltype(get_args(&F::operator()));

template<class F>
requires(not std::is_class_v<F>)
auto args() -> decltype(get_args(+std::declval<F>()));

template<class F>
using function_parameters_type = decltype(args<F>());


template<class F, class = std::enable_if_t<std::is_void_v<function_parameters_type<F>>>>
auto get_ret() -> decltype(std::invoke(std::declval<F>()));

template<class F, class = std::enable_if_t<not std::is_void_v<function_parameters_type<F>>>>
auto get_ret()
    -> decltype(std::apply(std::declval<F>(), std::declval<function_parameters_type<F>>()));

template<class F>
using function_return_type = decltype(get_ret<F>());

} // namespace func_traits


template<class T>
struct function_traits
{
    using argument_types = func_traits::function_parameters_type<T>;
    using return_type = func_traits::function_return_type<T>;
};


template<class T>
using first_argument_t = std::tuple_element_t<0, typename function_traits<T>::argument_types>;

} // namespace fossil::detail
