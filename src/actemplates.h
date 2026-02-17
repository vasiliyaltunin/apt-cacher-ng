#ifndef ACTEMPLATES_H
#define ACTEMPLATES_H

#include <functional>

namespace acng {

// dirty little RAII helper
struct tDtorEx {
        std::function<void(void)> _action;
        inline tDtorEx(decltype(_action) action) : _action(action) {}
        inline ~tDtorEx() { _action(); }
};

// unique_ptr semantics (almost) on a non-pointer type
template<typename T, void TFreeFunc(T), T inval_default>
struct auto_raii
{
	T m_p;
	auto_raii() : m_p(inval_default) {}
	explicit auto_raii(T xp) : m_p(xp) {}
	~auto_raii() { if (m_p != inval_default) TFreeFunc(m_p); }
	T release() { auto ret=m_p; m_p = inval_default; return ret;}
	T get() const { return m_p; }
	T& operator*() { return m_p; }
	auto_raii(const auto_raii&) = delete;
	auto_raii(auto_raii && other)
	{
		m_p = other.m_p;
		other.m_p = inval_default;
	}
	auto_raii& reset(auto_raii &&other)
	{
		if (&other == this)
			return *this;
		if (m_p != other.m_p)
		{
			if(valid())
				TFreeFunc(m_p);
			m_p = other.m_p;
		}
		other.m_p = inval_default;
		return *this;
	}
	void reset()
	{
		if (valid())
			TFreeFunc(m_p);
		m_p = inval_default;
	}
	bool valid() const { return inval_default != m_p;}
};


}


#endif // ACTEMPLATES_H
