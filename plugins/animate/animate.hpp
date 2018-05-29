#ifndef ANIMATE_H_
#define ANIMATE_H_

#include <view.hpp>
#include <wayfire/animation.hpp>

class animation_base
{
    public:
    virtual void init(wayfire_view view, wf_duration duration, bool close);
    virtual bool step(); /* return true if continue, false otherwise */
    virtual ~animation_base();
};

#endif
