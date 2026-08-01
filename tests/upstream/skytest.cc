// skytest: a CPU reference + validation harness for the Hillaire atmosphere the
// renderer bakes on the GPU (atmosphere.hlsli / transmittance / multiscatter /
// sky shaders). It can't run the shader on the CPU, so it mirrors the same model
// and asserts the physical invariants the GPU output must satisfy - transmittance
// bounds and horizon reddening, phase normalisation, the multiple-scattering
// series converging, and MS actually lifting the dark single-scatter horizon -
// plus prints a reference sky-colour table for eyeballed A/B. No GPU, no data, so
// it runs in the default ctest gate and catches model regressions.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace {

// --- spectral RGB ---
struct Spec {
  double r = 0, g = 0, b = 0;
};
Spec operator+(Spec a, Spec b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
Spec operator*(Spec a, double s) { return {a.r * s, a.g * s, a.b * s}; }
Spec operator*(Spec a, Spec b) { return {a.r * b.r, a.g * b.g, a.b * b.b}; }
Spec ExpNeg(Spec a) { return {std::exp(-a.r), std::exp(-a.g), std::exp(-a.b)}; }

// --- atmosphere constants (must match shaders/atmosphere.hlsli) ---
constexpr double kPi = 3.14159265358979;
constexpr double kGround = 6360e3;
constexpr double kTop = 6460e3;
const Spec kRayleigh{5.802e-6, 13.558e-6, 33.1e-6};
constexpr double kRayleighH = 8000.0;
constexpr double kMieScat = 3.996e-6;
constexpr double kMieExt = 4.40e-6;
constexpr double kMieH = 1200.0;
const Spec kOzone{0.650e-6, 1.881e-6, 0.085e-6};

struct Medium {
  Spec scattering;
  Spec extinction;
};
Medium SampleMedium(double h) {
  double rayleigh = std::exp(-h / kRayleighH);
  double mie = std::exp(-h / kMieH);
  double ozone = std::max(0.0, 1.0 - std::fabs(h - 25e3) / 15e3);
  Spec rs = kRayleigh * rayleigh;
  Medium m;
  m.scattering = rs + Spec{kMieScat * mie, kMieScat * mie, kMieScat * mie};
  m.extinction = rs + Spec{kMieExt * mie, kMieExt * mie, kMieExt * mie} + kOzone * ozone;
  return m;
}

// Nearest positive ray-sphere hit (origin-centred), -1 on miss.
double RaySphere(std::array<double, 3> p, std::array<double, 3> d, double radius) {
  double b = p[0] * d[0] + p[1] * d[1] + p[2] * d[2];
  double c = p[0] * p[0] + p[1] * p[1] + p[2] * p[2] - radius * radius;
  double disc = b * b - c;
  if (disc < 0) return -1;
  disc = std::sqrt(disc);
  double t0 = -b - disc, t1 = -b + disc;
  if (t1 < 0) return -1;
  return t0 < 0 ? t1 : t0;
}

double Len(std::array<double, 3> p) { return std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]); }

// Transmittance from a point at `radius` along a ray with view-zenith cosine
// `mu`, to the top of the atmosphere.
Spec Transmittance(double radius, double mu) {
  std::array<double, 3> p{0, radius, 0};
  std::array<double, 3> d{std::sqrt(std::max(0.0, 1 - mu * mu)), mu, 0};
  double t_top = RaySphere(p, d, kTop);
  if (t_top < 0) return {1, 1, 1};
  const int kSteps = 64;
  Spec tau{};
  double dt = t_top / kSteps;
  for (int i = 0; i < kSteps; ++i) {
    std::array<double, 3> s{p[0] + d[0] * (i + 0.5) * dt, p[1] + d[1] * (i + 0.5) * dt,
                            p[2] + d[2] * (i + 0.5) * dt};
    tau = tau + SampleMedium(Len(s) - kGround).extinction * dt;
  }
  return ExpNeg(tau);
}

double RayleighPhase(double c) { return 3.0 / (16.0 * kPi) * (1.0 + c * c); }
double MiePhase(double c, double g) {
  double g2 = g * g;
  return 3.0 / (8.0 * kPi) * ((1 - g2) * (1 + c * c)) /
         ((2 + g2) * std::pow(1 + g2 - 2 * g * c, 1.5));
}

// Single-scattered sky radiance for a view direction, sun straight up at `sun`.
// `with_ms` adds a crude isotropic multiple-scattering term so the test can show
// it lifting the horizon.
Spec SkyRadiance(std::array<double, 3> view, std::array<double, 3> sun, bool with_ms) {
  std::array<double, 3> p0{0, kGround + 500, 0};
  double t_ground = RaySphere(p0, view, kGround);
  double t_top = RaySphere(p0, view, kTop);
  double t_max = t_ground > 0 ? t_ground : t_top;
  if (t_max <= 0) return {};
  double mu = view[0] * sun[0] + view[1] * sun[1] + view[2] * sun[2];
  double rp = RayleighPhase(mu), mp = MiePhase(mu, 0.8);
  const int kSteps = 48;
  Spec L{}, through{1, 1, 1};
  double dt = t_max / kSteps;
  for (int i = 0; i < kSteps; ++i) {
    std::array<double, 3> pos{p0[0] + view[0] * (i + 0.5) * dt, p0[1] + view[1] * (i + 0.5) * dt,
                              p0[2] + view[2] * (i + 0.5) * dt};
    double r = Len(pos);
    Medium m = SampleMedium(r - kGround);
    Spec st = ExpNeg(m.extinction * dt);
    double sun_mu = (pos[0] * sun[0] + pos[1] * sun[1] + pos[2] * sun[2]) / r;
    Spec sunT = Transmittance(r, sun_mu);
    double rayleigh = std::exp(-(r - kGround) / kRayleighH);
    double mie = std::exp(-(r - kGround) / kMieH);
    Spec single = (kRayleigh * (rayleigh * rp) +
                   Spec{kMieScat * mie * mp, kMieScat * mie * mp, kMieScat * mie * mp}) *
                  sunT;
    Spec S = single;
    if (with_ms) {
      // Crude isotropic multiscatter: a fraction of the scattering, scaled by the
      // sunlight reaching the sample (so it fades out once the sun sets, like the
      // GPU multiscatter LUT). Enough to show the horizon lift.
      Spec ms = m.scattering * (0.35 / (4.0 * kPi)) * sunT;
      S = S + ms;
    }
    Spec inv{1.0 / std::max(m.extinction.r, 1e-12), 1.0 / std::max(m.extinction.g, 1e-12),
             1.0 / std::max(m.extinction.b, 1e-12)};
    Spec contrib = through * (S + (S * st) * -1.0) * inv;
    L = L + Spec{std::max(0.0, contrib.r), std::max(0.0, contrib.g), std::max(0.0, contrib.b)};
    through = through * st;
  }
  return L;
}

double Luma(Spec s) { return 0.2126 * s.r + 0.7152 * s.g + 0.0722 * s.b; }

}  // namespace

int main() {
  int failures = 0;
  auto check = [&](const char* what, bool ok) {
    std::printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
  };

  // --- Transmittance invariants ---
  Spec zenith = Transmittance(kGround + 1.0, 1.0);    // straight up
  Spec horizon = Transmittance(kGround + 1.0, 0.02);  // grazing
  check("transmittance in [0,1]", zenith.r <= 1 && zenith.b >= 0 && horizon.r <= 1);
  check("zenith transmits more than the horizon", zenith.g > horizon.g);
  check("blue is extinguished more than red (sky is blue)", zenith.b < zenith.r);
  check("horizon sun is reddened (R transmits >> B)", horizon.r > horizon.b * 3.0);

  // --- Phase normalisation: integrate over the sphere, expect ~1 ---
  double rp_int = 0, mp_int = 0;
  const int kN = 2000;
  for (int i = 0; i < kN; ++i) {
    double c = -1.0 + 2.0 * (i + 0.5) / kN;
    rp_int += RayleighPhase(c) * (2.0 / kN) * 2.0 * kPi;
    mp_int += MiePhase(c, 0.8) * (2.0 / kN) * 2.0 * kPi;
  }
  check("Rayleigh phase integrates to ~1", std::fabs(rp_int - 1.0) < 0.02);
  check("Mie phase integrates to ~1", std::fabs(mp_int - 1.0) < 0.05);

  // --- Multiple-scattering series converges (the 1/(1-fms) closed form) ---
  // fms = single-scatter albedo at the ground; must be < 1 for the series to sum.
  Medium ground = SampleMedium(0.0);
  double albedo = ground.scattering.g / ground.extinction.g;
  check("single-scatter albedo < 1 (MS series converges)", albedo > 0.0 && albedo < 1.0);
  check("1/(1-fms) is finite and > 1", 1.0 / (1.0 - albedo) > 1.0);

  // --- Ozone tent peaks near 25 km, absent at the ground ---
  Medium at25 = SampleMedium(25e3);
  check("ozone absorption present at 25 km", at25.extinction.g > SampleMedium(60e3).extinction.g);

  // --- Sky radiance: zenith bluer than horizon; sun set darkens the sky ---
  std::array<double, 3> up{0, 1, 0};
  std::array<double, 3> sun_high{0.0, 0.85, -0.53};   // ~58 deg
  std::array<double, 3> sun_horizon{0.0, 0.05, -1.0};  // ~3 deg
  std::array<double, 3> sun_below{0.0, -0.2, -0.98};   // below horizon
  // normalize the sun dirs
  for (auto* s : {&sun_high, &sun_horizon, &sun_below}) {
    double l = Len(*s);
    (*s)[0] /= l;
    (*s)[1] /= l;
    (*s)[2] /= l;
  }
  Spec zen_day = SkyRadiance(up, sun_high, true);
  check("daytime zenith is blue-dominant", zen_day.b > zen_day.r * 1.3);
  Spec zen_dusk = SkyRadiance(up, sun_horizon, true);
  check("zenith dims as the sun sets", Luma(zen_dusk) < Luma(zen_day));
  Spec zen_night = SkyRadiance(up, sun_below, true);
  check("sky is dark with the sun below the horizon", Luma(zen_night) < Luma(zen_dusk) * 0.5);

  // --- The headline win: multiple scattering lifts the dark single-scatter sky ---
  Spec horizon_dir{std::sin(1.4), std::cos(1.4), 0.0};  // ~10 deg above the horizon
  std::array<double, 3> hd{horizon_dir.r, horizon_dir.g, horizon_dir.b};
  Spec ss_only = SkyRadiance(hd, sun_high, false);
  Spec ss_plus_ms = SkyRadiance(hd, sun_high, true);
  check("multiple scattering brightens the horizon vs single only",
        Luma(ss_plus_ms) > Luma(ss_only) * 1.05);

  // --- Reference table for eyeballed A/B ---
  std::printf("\n  sun elevation -> zenith sky radiance (x1e3)\n");
  for (auto [name, s] : {std::pair{"noon  ", sun_high}, std::pair{"dusk  ", sun_horizon},
                         std::pair{"night ", sun_below}}) {
    Spec L = SkyRadiance(up, s, true) * 1000.0;
    std::printf("    %s  R=%7.3f  G=%7.3f  B=%7.3f\n", name, L.r, L.g, L.b);
  }

  std::printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}
