// C++ adaptation of perfect-freehand's default round-cap pipeline.
// Copyright (c) 2021 Stephen Ruiz Ltd. MIT; see third_party/perfect-freehand/LICENSE.
// Upstream: 176e00f2399f4969e1b0965c5921d96a3e50ce9f.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace freehand {
struct Vec { float x{}, y{}; };
inline Vec add(Vec a, Vec b) { return {a.x+b.x,a.y+b.y}; }
inline Vec sub(Vec a, Vec b) { return {a.x-b.x,a.y-b.y}; }
inline Vec mul(Vec a, float t) { return {a.x*t,a.y*t}; }
inline Vec lerp(Vec a, Vec b, float t) { return add(a,mul(sub(b,a),t)); }
inline float dot(Vec a, Vec b) { return a.x*b.x+a.y*b.y; }
inline float length(Vec a) { return std::hypot(a.x,a.y); }
inline Vec unit(Vec a) { float d=length(a); return d>0 ? mul(a,1/d) : Vec{}; }
inline Vec per(Vec a) { return {a.y,-a.x}; }
inline Vec rotate(Vec a, Vec center, float angle) {
    Vec d=sub(a,center); float s=std::sin(angle), c=std::cos(angle);
    return add(center,{d.x*c-d.y*s,d.x*s+d.y*c});
}
inline float pressure(float previous, float distance, float size) {
    float speed=std::min(1.0f,distance/size);
    return std::min(1.0f,previous+((1-speed)-previous)*(speed*0.275f));
}
struct Point { Vec position, vector; float distance{}, runningLength{}; };
struct Outline {
    std::vector<Vec> vertices;
    float pressureBeforeLast{};
};
// Default thinning=.5, smoothing=.5, identity easing, no taper.
// Caller carries pressure across bounded chunks; only the first chunk estimates it.
inline Outline outline(std::vector<Point> const& points, float size, float seed=-1, float widthScale=1) {
    Outline result;
    if (points.empty() || size<=0) return result;
    constexpr float pi=3.14169265359f;
    float previous=seed;
    if (previous<0) {
        previous=0.25f;
        for (size_t i=0;i<std::min<size_t>(10,points.size());++i)
            previous=(previous+pressure(previous,points[i].distance,size))*0.5f;
    }
    float radius=size*0.5f, firstRadius=0;
    Vec previousVector=points.front().vector;
    Vec previousLeft=points.front().position, previousRight=previousLeft;
    bool previousSharp=false;
    std::vector<Vec> left,right;
    // Apply compensation after pressure/radius calculation. Keep unscaled
    // candidates for edge filtering so sampling and smoothing do not change.
    auto display=[widthScale](Vec vertex, Vec center) { return lerp(center,vertex,widthScale); };
    float minDistance=size*size*0.25f;
    float total=points.back().runningLength;
    for (size_t i=0;i<points.size();++i) {
        auto const& p=points[i]; bool last=i+1==points.size();
        result.pressureBeforeLast=previous;
        if (!last && total-p.runningLength<3) continue;
        float current=pressure(previous,p.distance,size);
        radius=std::max(0.01f,size*(0.25f+0.5f*current));
        if (!firstRadius) firstRadius=radius;
        Vec next=last ? p.vector : points[i+1].vector;
        float nextDot=last ? 1 : dot(p.vector,next);
        bool sharp=dot(p.vector,previousVector)<0 && !previousSharp;
        bool nextSharp=nextDot<0;
        if (sharp || nextSharp) {
            Vec offset=mul(per(previousVector),radius);
            for (int j=0;j<=13;++j) {
                float t=static_cast<float>(j)/13;
                previousLeft=rotate(sub(p.position,offset),p.position,pi*t);
                previousRight=rotate(add(p.position,offset),p.position,-pi*t);
                left.push_back(display(previousLeft,p.position)); right.push_back(display(previousRight,p.position));
            }
            if (nextSharp) previousSharp=true;
            continue;
        }
        previousSharp=false;
        if (last) {
            Vec offset=mul(per(p.vector),radius);
            left.push_back(display(sub(p.position,offset),p.position)); right.push_back(display(add(p.position,offset),p.position));
            continue;
        }
        Vec offset=mul(per(lerp(next,p.vector,nextDot)),radius);
        Vec l=sub(p.position,offset), r=add(p.position,offset);
        Vec dl=sub(previousLeft,l), dr=sub(previousRight,r);
        if (i<=1 || dot(dl,dl)>minDistance) { left.push_back(display(l,p.position)); previousLeft=l; }
        if (i<=1 || dot(dr,dr)>minDistance) { right.push_back(display(r,p.position)); previousRight=r; }
        previous=current; previousVector=p.vector;
    }
    if (points.size()==1) {
        Vec center=points.front().position;
        for (int j=1;j<=26;++j) {
            float angle=2*pi*static_cast<float>(j)/26;
            result.vertices.push_back(add(center,{firstRadius*widthScale*std::cos(angle),firstRadius*widthScale*std::sin(angle)}));
        }
        return result;
    }
    result.vertices=std::move(left);
    Vec end=points.back().position;
    Vec capStart=add(end,mul(per(mul(points.back().vector,-1)),radius*widthScale));
    for (int j=1;j<29;++j)
        result.vertices.push_back(rotate(capStart,end,pi*3*static_cast<float>(j)/29));
    result.vertices.insert(result.vertices.end(),right.rbegin(),right.rend());
    for (int j=1;j<=13;++j)
        result.vertices.push_back(rotate(right.front(),points.front().position,pi*static_cast<float>(j)/13));
    return result;
}
}
