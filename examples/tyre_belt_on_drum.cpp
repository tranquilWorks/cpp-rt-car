// Legacy research experiment; not the Runtime SDK entry path. See README.md.
#include <cmath>
#include <iostream>
#include <numeric>
#include <vector>

int main() {
    const double drumRadius = 0.3; // metres
    const int samples = 360;       // angular samples around drum
    const double step = 2.0 * M_PI / samples;

    std::vector<double> tension(samples);
    for (int i = 0; i < samples; ++i) {
        double theta = i * step;
        // simple sinusoidal variation in belt tension
        tension[i] = 1000.0 + 100.0 * std::sin(theta);
    }

    double avg = std::accumulate(tension.begin(), tension.end(), 0.0) / samples;
    std::cout << "Average belt tension: " << avg << " N\n";
}
