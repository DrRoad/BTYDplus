#include <Rcpp.h>
#include <R_ext/Applic.h> // required to call Rdqags

using namespace Rcpp;

// slice sampling

// This is a stripped down C++ version of diversitree::mcmc thta only returns
// the draw from the last step. Drawing from gamma distribution is now 300x
// faster than compared with diversitree::mcmc.

// The algorithm is described in detail in Neal R.M. 2003. Slice sampling.
// Annals of Statistics 31:705-767. which describes *why* this algorithm works. 
// The approach differs from normal Metropolis-Hastings algorithms, and from
// Gibbs samplers, but shares the Gibbs sampler property of every update being
// accepted.  Nothing is required except the ability to evaluate the function at
// every point in continuous parameter space.

// Let x0 be the current (possibly multivariate) position in continuous
// parameter space, and let y0 be the probability at that point.  To update from
// (x0, y0) -> (x1, y1), we update each of the parameters in turn.  For each
// parameter
//
//   1. Draw a random number 'z' on Uniform(0, y0) -- the new point
//      must have at least this probability.
//
//   2. Find a region (x.l, x.r) that contains x0[i], such that x.l
//      and x.r are both smaller than z.
//
//   3. Randomly draw a new position from (x.l, x.r).  If this
//      position is greater than z, this is our new position.
//      Otherwise it becomes a new boundary and we repeat this step
//      (the point x1 becomes x.l if x.l < x0[i], and x.r otherwise so
//      that x0[i] is always contained within the interval).
// Because it is generally more convenient to work with log
// probabilities, step 1 is modified so that we draw 'z' by taking
//   y0 - rexp(1)
// All other steps remain unmodified.

NumericVector slice_sample_cpp(double (*logfn)(NumericVector, NumericVector), 
                        NumericVector params,
                        NumericVector x0, 
                        int steps = 10, 
                        double w = 1,
                        double lower = -INFINITY, 
                        double upper = INFINITY) {

  double u, r0, r1, logy, logz, logys;
  NumericVector x, xs, L, R;
  
  x = clone(x0);
  L = clone(x0);
  R = clone(x0);
  logy = logfn(x, params);

  for (int i = 0; i < steps; i++) {
    
    for (int j = 0; j < x0.size(); j++) {
      // draw uniformly from [0, y]
      logz = logy - rexp(1)[0];
      
      // expand search range
      u = runif(1)[0] * w;
      L[j] = x[j] - u;
      R[j] = x[j] + (w-u);
      while ( L[j] > lower && logfn(L, params) > logz )
        L[j] = L[j] - w;
      while ( R[j] < upper && logfn(R, params) > logz )
        R[j] = R[j] + w;
      
      // sample until draw is within valid range
      r0 = std::max(L[j], lower);
      r1 = std::min(R[j], upper);

      xs = clone(x);
      do {
        xs[j] = runif(1, r0, r1)[0];
        logys = logfn(xs, params);
        if ( logys > logz ) 
          break;
        if ( xs[j] < x[j] )
          r0 = xs[j];
        else
          r1 = xs[j];
      } while (true);
      
      x = clone(xs);
      logy = logys;
    }
  }
  
  return x;
}

// draw from gamma distribution (for test purposes)

double post_gamma(NumericVector x, NumericVector params) {
  double alpha = params[0];
  double beta = params[1];
  return (alpha - 1) * log(x[0]) - beta * x[0];
}

// [[Rcpp::export]]
NumericVector slice_sample_gamma(double alpha, double beta, double lower, double upper) {
  NumericVector params = NumericVector::create(alpha, beta);
  NumericVector x0 = NumericVector::create(alpha/beta);
  double steps = 10;
  double w = 3 * sqrt(alpha) / beta; // approx size of (q95-q05)
  return slice_sample_cpp(post_gamma, params, x0, steps, w, lower, upper);
}

// draw from multivariate normal distribution (for test purposes)

double post_mvnorm(NumericVector x, NumericVector sigma) {
  return -log(2*3.141593) -0.5 * log(sigma[0]*sigma[3]-sigma[1]*sigma[2]) -0.5 * (1/(sigma[0]*sigma[3]-sigma[1]*sigma[2])) * 
    (x[0]*x[0]*sigma[3] - x[0]*x[1]*sigma[2] - x[0]*x[1]*sigma[1] + x[1]*x[1]*sigma[0]);
}

// [[Rcpp::export]]
NumericVector slice_sample_mvnorm(NumericVector sigma) {
  NumericVector x0 = NumericVector::create(0.2, 0.3);
  double steps = 20;
  return slice_sample_cpp(post_mvnorm, sigma, x0, steps);
}

// estimate parameters of gamma distribution

double post_gamma_parameters(NumericVector log_x, NumericVector params) {
  double shape = exp(log_x[0]);
  double rate = exp(log_x[1]);
  double len_x = params[0];
  double sum_x = params[1];
  double sum_log_x = params[2];
  double hyper1 = params[3];
  double hyper2 = params[4];
  double hyper3 = params[5];
  double hyper4 = params[6];
  return len_x * (shape * log(rate) - lgamma(shape)) + (shape-1) * sum_log_x - rate * sum_x + 
    (hyper1 - 1) * log(shape) - (shape * hyper2) +
    (hyper3 - 1) * log(rate) - (rate * hyper4);
}

// [[Rcpp::export]]
NumericVector slice_sample_gamma_parameters(NumericVector data, NumericVector init, 
                                            NumericVector hyper, double steps = 20, double w = 1) {
  NumericVector params = NumericVector::create(data.size(), sum(data), sum(log(data)), 
                                                hyper[0], hyper[1], hyper[2], hyper[3]);
  return exp(slice_sample_cpp(post_gamma_parameters, params, log(init), steps, w, -INFINITY, INFINITY));
}

/*** R

  # unit-test slice sampling for univariate distribution
  alpha <- 2
  beta <- 5
  n <- 1e4
  lower <- 0.3
  upper <- 0.8
  draws1 <- sapply(1:n, function(i) slice_sample_gamma(alpha, beta, lower, upper))
  draws2 <- rgamma(n, alpha, beta)
  draws2 <- draws2[draws2>lower & draws2<upper]
  stopifnot(abs(mean(draws1)-mean(draws2))<.1)

  # unit-test slice sampling for bivariate distribution
  n <- 1e4
  sigma <- c(1, 0.6, 0.6, 1.2)
  draws1 <- t(sapply(1:n, function(i) slice_sample_mvnorm(sigma)))
  draws2 <- MASS::mvrnorm(n, mu=c(0,0), Sigma=matrix(sigma, ncol=2))
  stopifnot(mean(abs(cov(draws2) - cov(draws1)))<.3)

  # unit-test slice_sample_gamma_parameters
  n <- 1e4
  params <- c(1.4, 3.5)
  x <- rgamma(1e4, params[1], params[2])
  draws <- t(replicate(n, slice_sample_gamma_parameters(x, c(1,1), rep(1e-3, 4))))
  stopifnot(max(abs(apply(draws, 2, mean) - params))<.1)
*/


// 
// working R implementation of slice sampling
//
//slice_sample <- function(logfn, x0, steps, w, lower=-Inf, upper=Inf) {
//  
//  x <- x0
//  logy <- logfn(x0)
//  
//  for (i in 1:steps) {
//    # draw uniformly from [0, y]
//    logz <- logy - rexp(1)
//    
//    # expand search range
//    u <- runif(1) * w
//    L <- x - u
//    R <- x + (w-u)
//    while ( L > lower && logfn(L) > logz )
//      L <- L - w
//    while ( R < upper && logfn(R) > logz )
//      R <- R + w
//    
//    # sample until draw is within valid range
//    r0 <- max(L, lower)
//    r1 <- min(R, upper)
//    repeat {
//      xs <- runif(1, r0, r1)
//      logys <- logfn(xs)
//      if ( logys > logz ) break
//      else if ( xs < x )  r0 <- xs
//      else                r1 <- xs
//    }
//    x <- xs
//    logy <- logys
//  }
//  
//  return(x)
//}






// ********* Pareto / NBD **********


// draw of individual-level posterior for Pareto/NBD (Ma/Liu)

double post_lambda_ma_liu(NumericVector data, NumericVector params) {
  double lambda_ = data[0];
  double x       = params[0];
  double tx      = params[1];
  double Tcal    = params[2];
//  double lambda = params[3];
  double mu      = params[4];
  double r       = params[5];
  double alpha   = params[6];
//  double s       = params[7];
//  double beta    = params[8];
  return (r-1) * log(lambda_) - (lambda_*alpha) +
    x * log(lambda_) - log(lambda_+mu) + 
    log(mu*exp(-tx*(lambda_+mu))+lambda_*exp(-Tcal*(lambda_+mu)));
}

double post_mu_ma_liu(NumericVector data, NumericVector params) {
  double mu_    = data[0];  
  double x      = params[0];
  double tx     = params[1];
  double Tcal   = params[2];
  double lambda = params[3];
//  double mu     = params[4];
//  double r      = params[5];
//  double alpha  = params[6];
  double s      = params[7];
  double beta   = params[8];
  return (s-1) * log(mu_) - (mu_*beta) +
    x * log(lambda) - log(lambda+mu_) + 
    log(mu_*exp(-tx*(lambda+mu_))+lambda*exp(-Tcal*(lambda+mu_)));
}

// [[Rcpp::export]]
NumericVector slice_sample_ma_liu(String what, 
                                  NumericVector x, NumericVector tx, NumericVector Tcal,
                                  NumericVector lambda, NumericVector mu,
                                  double r, double alpha, double s, double beta) {
  int N = x.size();
  NumericVector out(N);
  for (int i=0; i<N; i++) {
    NumericVector params = NumericVector::create(x[i], tx[i], Tcal[i], lambda[i], mu[i], r, alpha, s, beta);
    if (what == "lambda") {
      out[i] = slice_sample_cpp(post_lambda_ma_liu, params, lambda[i], 3, 3 * sqrt(r) / alpha, 0, INFINITY)[0];
    } else if (what == "mu") {
      out[i] = slice_sample_cpp(post_mu_ma_liu, params, mu[i], 6, 3 * sqrt(s) / beta, 0, INFINITY)[0];
    }
  }
  return out;
}




// ********* Pareto / CNBD **********


// draw of individual-level posterior for Pareto/CNBD

// see http://www.hep.by/gnu/r-patched/r-exts/R-exts_143.html
//   for how to integrate within Rcpp
typedef void integr_fn(double *x, int n, void *ex);

void pcnbd_palive_integrand(double *x, int n, void *ex) {
  double *params;
  params = (double*)ex; // array of type 'double' and length 4
  double tx = *(params+1);
  double k = *(params+3);
  double lambda = *(params+4);
  double mu = *(params+5);
  for(int i=0; i<n; i++) {
    // call pgamma with lower.tail=FALSE and log.p=FALSE; note that 3rd parameter is scale and not rate;
    x[i] = ::Rf_pgamma(x[i]-tx, k, 1/(k*lambda), 0, 0) * exp(-mu*x[i]);
  }
}

// [[Rcpp::export]]
NumericVector pcnbd_palive(NumericVector x, NumericVector tx, NumericVector Tcal, 
                           NumericVector k, NumericVector lambda, NumericVector mu) {
  int N = x.size();
  NumericVector out(N);
  for (int i=0; i<N; i++) {
    // calc numerator
    double one_minus_F = ::Rf_pgamma(Tcal[i]-tx[i], k[i], 1/(k[i]*lambda[i]), 0, 0);
    double numer = one_minus_F * exp(-mu[i]*Tcal[i]);
    // calc denominator (by integrating from tx to Tcal)
    double denom;
    void *ex;
    double fn_params[6] = {x[i], tx[i], Tcal[i], k[i], lambda[i], mu[i]};
    ex = &fn_params;
    double epsabs = 0.0001;
    double epsrel = 0.0001;
    double abserr = 0.0;
    int neval = 0;
    int ier = 0;
    int limit = 100; // = subdivisions parameter
    int lenw = 4 * limit;
    int last = 0;
    int iwork = limit;
    double work = 4.0 * limit;
    Rdqags(pcnbd_palive_integrand, ex, &tx[i], &Tcal[i],
           &epsabs, &epsrel, &denom, &abserr,
           &neval, &ier, &limit, &lenw, &last, &iwork, &work); // result is return to denom variable
    denom = numer + mu[i] * denom;
    out[i] = (numer/denom);
  }
  return(out);
}


double pcnbd_post_tau(NumericVector data, NumericVector params) {
  double tau_    = data[0];
//  double x      = params[0];
//  double tx     = params[1];
//  double Tcal   = params[2];
//  double litt   = params[3];
  double k      = params[4];
  double lambda = params[5];
  double mu     = params[6];
//  double tau    = params[7];
//  double t      = params[8];
//  double gamma  = params[9];
//  double r      = params[10];
//  double alpha  = params[11];
//  double s      = params[12];
//  double beta   = params[13];  
  
  double one_minus_F = ::Rf_pgamma(tau_, k, 1/(k*lambda), 0, 0);
  double f = ::Rf_dgamma(tau_, k, 1/(k*lambda), 0);
  return(-mu*tau_ + log(mu*one_minus_F + f));
}


double pcnbd_post_k(NumericVector data, NumericVector params) {
  double k_     = data[0];
  double x      = params[0];
  double tx     = params[1];
  double Tcal   = params[2];
  double litt   = params[3];
//  double k      = params[4];
  double lambda = params[5];
//  double mu     = params[6];
  double tau    = params[7];
  double t      = params[8];
  double gamma  = params[9];
//  double r      = params[10];
//  double alpha  = params[11];
//  double s      = params[12];
//  double beta   = params[13];
  
  double log_one_minus_F = ::Rf_pgamma(std::min(Tcal, tau) - tx, k_, 1/(k_*lambda), 0, 1);
  return (t-1) * log(k_) - (k_*gamma) +
    k_ * x * log(k_*lambda) - x * lgamma(k_) - k_ * lambda * tx + (k_-1) * litt + 
    log_one_minus_F;
}

double pcnbd_post_lambda(NumericVector data, NumericVector params) {
  double lambda_ = data[0];
  double x       = params[0];
  double tx      = params[1];
  double Tcal    = params[2];
//  double litt    = params[3];
  double k       = params[4];
//  double lambda  = params[5];
//  double mu      = params[6];
  double tau     = params[7];
//  double t       = params[8];
//  double gamma   = params[9];
  double r       = params[10];
  double alpha   = params[11];
//  double s       = params[12];
//  double beta    = params[13];  
  
  double log_one_minus_F = ::Rf_pgamma(std::min(Tcal, tau) - tx, k, 1/(k*lambda_), 0, 1);
  return (r-1) * log(lambda_) - (lambda_*alpha) +
    k * x * log(lambda_) - k * lambda_ * tx +
    log_one_minus_F;
}


// [[Rcpp::export]]
NumericVector pcnbd_slice_sample(String what, 
                                  NumericVector x, NumericVector tx, NumericVector Tcal, NumericVector litt,
                                  NumericVector k, NumericVector lambda, NumericVector mu, NumericVector tau,
                                  double t, double gamma, double r, double alpha, double s, double beta) {
  int N = x.size();
  NumericVector out(N);
  
  for (int i=0; i<N; i++) {
    NumericVector params = NumericVector::create(
                              x[i], tx[i], Tcal[i], litt[i], 
                              k[i], lambda[i], mu[i], tau[i], 
                              t, gamma, r, alpha, s, beta);
    //Rcpp::Rcout << i << " - " << x[i] << " - " << tx[i] << " - " << Tcal[i] << " - " << litt[i] << " - " << k[i] << " - " << lambda[i] << " - " << mu[i] << " - " << tau[i] << " - " << t << " - " << gamma << " - " << r << " - " << alpha << " - " << s << " - " << beta << " - " << std::endl;
    if (what == "k") {
      out[i] = slice_sample_cpp(pcnbd_post_k, params, NumericVector::create(k[i]), 3, 3 * sqrt(t) / gamma, 0, INFINITY)[0];
    } else if (what == "lambda") {
      out[i] = slice_sample_cpp(pcnbd_post_lambda, params, NumericVector::create(lambda[i]), 3, 3 * sqrt(r) / alpha, 0, INFINITY)[0];
    } else if (what == "tau") {
      if (::Rf_pgamma(tx[i], k[i], 1/(k[i]*lambda[i]), 0, 1) < -100) {
        // distribution is 'too flat' to sample properly, so we draw uniformly
        out[i] = runif(1, tx[i], Tcal[i])[0];
      } else {
        double tau_init;
        if (tau[i] > Tcal[i] || tau[i] < tx[i]) {
          tau_init = tx[i] + (Tcal[i]-tx[i])/2;
        } else {
          tau_init = tau[i];
        }
        out[i] = slice_sample_cpp(pcnbd_post_tau, params, NumericVector::create(tau_init), 6, (Tcal[i]-tx[i])/2, tx[i], Tcal[i])[0];
      }
    }
  }
  return out;
}

/*** R
  # unit-test slice sampling of pcnbd_post_tau, by comparing results to pareto/nbd (k=1), 
  #   where we can draw directly via http://en.wikipedia.org/wiki/Inverse_transform_sampling
  x <- 0
  tx <- 8
  Tcal <- 14
  litt <- 0
  k <- 1
  lambda <- 1.2
  mu <- 0.01
  n <- 10^4
  draws1 <- pcnbd_slice_sample("tau", rep(x, n), rep(tx, n), rep(Tcal, n), rep(litt, n), 
                               rep(k, n), rep(lambda, n), rep(mu, n), rep(0, n), 
                               0,0,0,0,0,0)
  rand <- runif(n)
  draws2 <- -log( (1-rand)*exp(-(mu+lambda) * tx) + rand*exp(-(mu+lambda) * Tcal)) / (mu+lambda)
  err <- abs(mean(draws1)-mean(draws2))
  stopifnot(err<.1)
*/

/*** R
  # unit-test P(alive) by comparing C++ result matches R results, matches Pareto/NBD result (k=1)
  x <- 0
  tx <- 7
  Tcal <- 12
  k <- 1
  lambda <- 1.4
  mu <- 0.015
  # C++ implementation
  res1 <- pcnbd_palive(x, tx, Tcal, k, lambda, mu)
  # R implementation
  ff <- function(y, tx, k, lambda, mu) (1-pgamma(y-tx, k, k*lambda)) * exp(-mu*y)
  res2 <- ff(Tcal, tx, k, lambda, mu) / (ff(Tcal, tx, k, lambda, mu) + 
                                   mu * integrate(ff, lower=tx, upper=Tcal, tx=tx, k=k, lambda=lambda, mu=mu)$value)
  # R implementation for k=1
  la_mu <- lambda + mu
  res3 <- exp(-la_mu*Tcal) / (exp(-la_mu*Tcal) + (mu/la_mu)*(exp(-la_mu*tx)-exp(-la_mu*Tcal)))

  stopifnot(round(res1, 4)==round(res2, 4))
  stopifnot(k!=1 | round(res1, 4)==round(res3, 4))
*/