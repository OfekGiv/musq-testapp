| cores | work | layout | strict max Mpps (busy avg / max) | next step fails | saturation run | strict / saturation |
|---|---|---|---|---|---|---|
| 2 | fixed:100 | distributor | **7** (86 / 100 %) | 7.25: RX, RX loss 3.023 % | 7.05 | -1 % |
| 2 | fixed:100 | mu_sq | **11** (100 / 100 %) | 11.25: TX+RX, refused 0.019 %, RX loss 0.018 % | 11.75 | -6 % |
| 4 | fixed:100 | distributor | **21** (96 / 100 %) | 21.5: RX, RX loss 1.754 % | 21.15 | -1 % |
| 4 | fixed:100 | mu_sq | **22** (100 / 100 %) | 22.5: TX+RX, refused 0.024 %, RX loss 0.012 % | 23.24 | -5 % |
| 4 | fixed:100 | plain RSS | **25** (100 / 100 %) | 25.5: RX, RX loss 1.501 % | 25.80 | -3 % |
| 4 | fixed:50 | distributor | **34** (99 / 100 %) | 34.5: RX, RX loss 0.396 % | 34.97 | -3 % |
| 4 | fixed:50 | mu_sq | **33** (100 / 100 %) | 33.5: TX, refused 0.013 % | 34.80 | -5 % |
| 8 | fixed:100 | distributor | **34** (76 / 100 %) | 34.5: RX, RX loss 0.056 % | 34.75 | -2 % |
| 8 | fixed:100 | mu_sq | **39.5** (94 / 94 %) | 40: TX, refused 0.266 % | 37.69 | +5 % |
| 8 | fixed:200 | distributor | **30.5** (99 / 100 %) | 31: RX, RX loss 1.341 % | 30.56 | -0 % |
| 8 | fixed:200 | mu_sq | **30** (100 / 100 %) | 30.5: TX, refused 0.011 % | 31.08 | -3 % |
| 8 | fixed:200 | plain RSS | **32** (100 / 100 %) | 32.5: RX, RX loss 0.284 % | 32.95 | -3 % |
