#pragma once

namespace huskyfe {


struct Spring {
    float value     = 1.0f;
    float velocity  = 0.0f;
    float target    = 1.0f;
    float stiffness = 380.0f;
    float damping   =  14.0f;

    void set(float t)        { target = t; }
    void snap_to(float v)    { value = v; target = v; velocity = 0.0f; }
    bool settled(float eps = 1e-3f) const {
        return (value > target ? value - target : target - value) < eps
            && (velocity > -eps && velocity < eps);
    }

    void tick(float dt) {

        if (dt > 1.0f / 30.0f) dt = 1.0f / 30.0f;


        constexpr float MAX_SUB_DT = 1.0f / 240.0f;
        int steps = (int)(dt / MAX_SUB_DT) + 1;
        float sub  = dt / (float)steps;
        for (int i = 0; i < steps; i++) {
            float force = -stiffness * (value - target) - damping * velocity;
            velocity += force * sub;
            value    += velocity * sub;
        }
    }
};

}
