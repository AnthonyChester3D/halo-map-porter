// Halo Map Porter - what an effect costs the Xbox (effects.hpp).
#include "effects.hpp"

#include <algorithm>
#include <cmath>

#include "tagfile.hpp"

namespace fx {

namespace {

struct Particle {
    double life = 0;
    std::string death, collision;
};

// A whole run's totals, for an effect another one makes.
struct Totals {
    double alive_seconds = 0, area_seconds = 0, time = 0;
};

}   // namespace

struct Analyzer::Impl {
    Find find;
    std::map<std::string, Cost> costs;
    std::map<std::string, Totals> totals;
    std::map<std::string, Particle> particles;
    std::set<std::string> busy;   // (cycle guard)

    const Particle &particle(const std::string &tag) {
        auto it = particles.find(tag);
        if (it != particles.end()) return it->second;
        Particle p;
        tagfile::Tag t;
        std::filesystem::path f = tag.empty() ? std::filesystem::path() : find(tag);
        if (!f.empty() && t.load(f)) {
            tagfile::Elem r = t.root();
            p.life = std::max(r.num("lifespan", 0), r.num("lifespan", 1));
            p.death = r.ref("death effect");
            p.collision = r.ref("collision effect");
        }
        return particles[tag] = p;
    }

    void compute(const std::string &tag) {
        if (costs.count(tag) || busy.count(tag)) return;
        busy.insert(tag);
        Cost c;
        Totals tot;
        tagfile::Tag t;
        std::filesystem::path f = tag.empty() ? std::filesystem::path() : find(tag);
        if (!f.empty() && t.load(f)) {
            tagfile::Elem r = t.root();
            tagfile::Block ev = r.block("events");
            int ls = (int)(int16_t)r.integer("loop start event"), le = (int)(int16_t)r.integer("loop stop event");
            bool ranged = ls >= 0 && ls < (int)ev.size();
            double loop_alive_s = 0, loop_area_s = 0;
            for (size_t i = 0; i < ev.size(); i++) {
                double w = 1 - std::min(1.0, std::max(0.0, ev[i].num("skip fraction")));
                double delay = std::max(ev[i].num("delay bounds", 0), ev[i].num("delay bounds", 1));
                double dur = std::max(ev[i].num("duration bounds", 0), ev[i].num("duration bounds", 1));
                double spread = std::max(dur, 1.0 / 30);   // (particles are made over the event's duration; at least a tick)
                double ev_alive_s = 0, ev_area_s = 0;
                // Effects its parts make, alongside.
                tagfile::Block parts = ev[i].block("parts");
                for (size_t k = 0; k < parts.size(); k++) {
                    std::string g, ref = parts[k].ref("type", &g);
                    if (g != "effe" || ref.empty()) continue;
                    compute(ref);
                    const Cost &cc = costs[ref];
                    const Totals &ct = totals[ref];
                    c.peak_alive += w * cc.peak_alive;
                    c.peak_area += w * cc.peak_area;
                    c.particles += w * cc.particles;
                    ev_alive_s += w * ct.alive_seconds;
                    ev_area_s += w * ct.area_seconds;
                }
                tagfile::Block ps = ev[i].block("particles");
                for (size_t k = 0; k < ps.size(); k++) {
                    std::string pt = ps[k].ref("particle type");
                    if (pt.empty()) continue;
                    double n = std::max(ps[k].num("count", 0), ps[k].num("count", 1)) * w;
                    double rad = std::max(ps[k].num("radius", 0), ps[k].num("radius", 1));
                    const Particle p = particle(pt);
                    double at_once = n * std::min(1.0, p.life / spread);
                    c.peak_alive += at_once;
                    c.peak_area += at_once * rad * rad;
                    c.particles += n;
                    ev_alive_s += n * p.life;
                    ev_area_s += n * p.life * rad * rad;
                    // Effects its particles make when they hit something or die: about as many at once as are alive.
                    for (const std::string &x : {p.death, p.collision}) {
                        if (x.empty()) continue;
                        compute(x);
                        const Cost &cc = costs[x];
                        const Totals &ct = totals[x];
                        c.peak_alive += at_once * cc.peak_alive;
                        c.peak_area += at_once * cc.peak_area;
                        c.particles += n * cc.particles;
                        ev_alive_s += n * ct.alive_seconds;
                        ev_area_s += n * ct.area_seconds;
                    }
                }
                tot.alive_seconds += ev_alive_s;
                tot.area_seconds += ev_area_s;
                tot.time += delay + dur;
                // An attached effect repeats its loop range, or all of it when it has none.
                if (!ranged || ((int)i >= ls && (int)i <= std::max(ls, le))) {
                    c.cycle += delay + dur;
                    loop_alive_s += ev_alive_s;
                    loop_area_s += ev_area_s;
                }
            }
            double cyc = std::max(c.cycle, 1.0 / 30);   // (a stopped loop starts again on the next tick)
            c.steady_alive = loop_alive_s / cyc;
            c.steady_area = loop_area_s / cyc;
        }
        busy.erase(tag);
        costs[tag] = c;
        totals[tag] = tot;
    }
};

Analyzer::Analyzer(Find find) : d(new Impl) { d->find = std::move(find); }
Analyzer::~Analyzer() { delete d; }

Cost Analyzer::cost(const std::string &effect) {
    d->compute(effect);
    return d->costs[effect];
}

std::set<std::string> Analyzer::part_effects(const std::string &effect) {
    std::set<std::string> out;
    tagfile::Tag t;
    std::filesystem::path f = effect.empty() ? std::filesystem::path() : d->find(effect);
    if (f.empty() || !t.load(f)) return out;
    tagfile::Block ev = t.root().block("events");
    for (size_t i = 0; i < ev.size(); i++) {
        tagfile::Block parts = ev[i].block("parts");
        for (size_t k = 0; k < parts.size(); k++) {
            std::string g, ref = parts[k].ref("type", &g);
            if (g == "effe" && !ref.empty()) out.insert(ref);
        }
    }
    return out;
}

double keep_share(const Cost &c, bool looping, const Caps &caps) {
    double s = 1;
    auto fit = [&](double v, double cap) {
        if (v > cap && v > 0) s = std::min(s, cap / v);
    };
    if (looping) {
        fit(c.steady_alive, caps.steady_alive);
        fit(c.steady_area, caps.steady_area);
    } else {
        fit(c.peak_alive, caps.peak_alive);
        fit(c.peak_area, caps.peak_area);
    }
    return s;
}

bool scale_particles(const std::filesystem::path &file, double share) {
    if (!(share < 1)) return false;
    tagfile::EditTag t;
    if (!t.load(file)) return false;
    auto *ev = tagfile::block(t.root(), "events");
    if (!ev) return false;
    bool changed = false;
    for (auto &e : *ev) {
        auto *ps = tagfile::block(e, "particles");
        if (!ps) continue;
        for (auto &p : *ps) {
            double lo = tagfile::get_num(p, "count", 0), hi = tagfile::get_num(p, "count", 1);
            if (hi < lo) std::swap(lo, hi);
            double nh = hi >= 1 ? std::max(1.0, std::round(hi * share)) : hi;
            double nl = std::min(nh, std::round(lo * share));
            if (nh == hi && nl == lo) continue;
            tagfile::set_num(p, "count", nl, 0);
            tagfile::set_num(p, "count", nh, 1);
            changed = true;
        }
    }
    return changed && t.save(file);
}

}   // namespace fx
