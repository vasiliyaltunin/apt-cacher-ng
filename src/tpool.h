#ifndef TPOOL_H
#define TPOOL_H

#include <functional>
#include <memory>

namespace acng {

class tpool
{
public:
	tpool() =default;
	virtual ~tpool() =default;
	virtual bool schedule(std::function<void()>) =0;
	virtual void stop() =0;
	static std::shared_ptr<tpool> Create(unsigned maxCount, unsigned maxSpare);
};
}

#endif // TPOOL_H
