W,H,MAXI = 200,200,60
total = 0
for py in range(H):
    y0 = py*2.5/H - 1.25
    for px in range(W):
        x0 = px*3.0/W - 2.0
        x = y = 0.0
        i = 0
        while i < MAXI:
            xx = x*x; yy = y*y
            if xx+yy > 4.0:
                i = MAXI
            else:
                xt = xx-yy+x0
                y = 2.0*x*y + y0
                x = xt
                i += 1
                total += 1
print(total)
