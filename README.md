Mark plug 6, Eric's middle 2, Jeff's last 5


firebase deploy --only hosting,database



Steps for OTA:
THE CHIP HAS TO BE BURNED WITH THE DEVICEID SET TO reefDoser1,2,3,4,5,etc
 build the scetch and find in under C:\Users\mdroo\OneDrive\Documents\platformio\AIDoser\.pio\build\esp32doit-devkit-v1/firmware.bin

 Move that firmware.bin file to C:\Users\mdroo\OneDrive\Firebase\aidoser\public\devices\reefDoser5 or 1 or 2...

 open dos prompt navigate to C:\Users\mdroo\OneDrive\Firebase\aidoser\public> 
 and type firebase deploy
 then on GUI select file in .....dev
 ices/reefDoserx(1,2,3...) 
 push the button Update firmware &  request OTA

 ///////////////to create a new user  /////////////////////

 Create a user in Firebase Authenticaiton.
 Use that user's key and create at user under aidoser/users  using the key just created
 create the devce under aidoser/devices/reefDoser6 (get file from firebase stuff folder in project Add_new_device.json).

 

 







IMPORTANT NOTES for AIDosder

When to use reset AI

Good times to flip resetAi to true:

After you change tank volume (add/remove big equipment, frag tank, sump)

After a big chemistry correction done manually (huge water change, big buffer dump)

If dosing has been weird because of bad test data and you want to restart the learning

file has to go under otaRequest payload: https://aidoser.web.app/devices/reefDoser5/firmware.bin


5.6 / day

run 15 ml

run 12 hours run 48 min/hour little pump

eric buckets:
Kalc  needs to be air tight
calcium carbonate   Carolina chemicals
alk(lye)[sodium hydroxide]  strong amazon i have these. 

trace elements:   

everyone doses alk and calcium mg?

sellable powder.
masses			once brs turn bs
 brs alk -> -->  sodium hydroxide don't need to be air tight
brs calcium -->  calcium carbinate from Carolina  don't need to be air tight# AIDoser

Things to do:



build calibration page.

do multiplication for dosing parameters for starting.

//////////////Kalk dkh/ml ///////////////////////

📦 300 gallons tank

First convert gallons to liters:

300 gal × 3.785 L/gal = 1,135.5 L

Now compare to baseline:

1,135.5 L ÷ 100 L = 11.355× more volume

So the same 1 mL dose will give:

0.0014
11.355
=
0.000123
 dKH per mL
11.355
0.0014
	​

=0.000123 dKH per mL
Final number for 300 gal system:


//////////////// calium in Kalk  ppm/ml

📦 Tank water volume

Your tank: 300 gallons

Convert:

300
×
3.785
=
1135.5
 L
300×3.785=1135.5 L
📍 Now compute ppm rise per 1 mL dose

We add 0.8 mg Ca into 1135.5 L:

ppm
=
0.8
1135.5
=
0.000704
 ppm
ppm=
1135.5
0.8
	​

=0.000704 ppm
✔ Final Answer:

1 mL saturated kalk → ~0.00070 ppm Ca in a 300-gallon system

////////////// dkh/ml in sodium hydroxide  ///////////////////////////
Perfect — this one is clean because sodium hydroxide (NaOH) is a pure alkalinity reagent (no calcium). So we can compute its dKH per mL for your 300-gallon (≈1135.6 L) system once we know the solution strength.

But I need one clarification from you:

What concentration sodium hydroxide are you using?

There are common hobby concentrations:

1 N NaOH (same strength as BRS / many 2-part systems)

2 N NaOH

½ N

Homemade by weight (e.g., 40 g in 1 L) — yields ~1 N

Full-saturated (≈19 wt%) — dangerous and not typical for dosing

If you're using the standard reef dosing version (1 N)

I’ll compute assuming 1 N NaOH, since that’s what commercial reef “ALK Part” usually is.

🧮 Alkalinity of 1 N NaOH

1 N NaOH = 1 mol/L = 1 equivalent/L of alkalinity

1 equivalent = 50,000 dKH/L

So per mL:

50
,
000
÷
1000
=
50
 dKH per mL (in pure water)
50,000÷1000=50 dKH per mL (in pure water)

This is the "reagent strength."
Then we dilute into your tank volume:

🧮 DKH per mL in a 300-gallon system

Tank volume = 1135.6 L

Add 1 mL dosing solution → adds 50 dKH worth of alkalinity into:

Tank dKH increase
=
50
1135.6
=
0.044
 dKH
Tank dKH increase=
1135.6
50
	​

=0.044 dKH
✅ Final result for 1 mL of 1N NaOH into 300 gal reef

1 mL of 1 N sodium hydroxide → +0.044 dKH in a 300-gallon tank

////////////////////// alk in calcium carbonate /////////////////////

🧪 Chemistry Basis

CaCO₃ provides:

1 mole Ca²⁺

2 equivalents alkalinity (i.e., 2 moles HCO₃⁻)

Molecular weights:

CaCO₃ = 100 g/mol

1 mole CaCO₃ → 2 eq alkalinity

So alkalinity per gram:

2
 eq
100
 g
=
0.02
 eq/g
100 g
2 eq
	​

=0.02 eq/g

Convert eq → dKH:

0.02
 eq/g
×
50,000
 dKH/eq
=
1000
 dKH/g
0.02 eq/g×50,000 dKH/eq=1000 dKH/g

So the reagent strength is:

1 g CaCO₃ = 1000 dKH (pure)

And:

1 mg CaCO₃ = 1 dKH

This is a very clean conversion.

🧮 Now convert for your tank volume = 300 gal

300 gal = 1135.6 L

If you add 1 mg CaCO₃ to 1135.6 L:

Tank increase
=
1
 dKH
1135.6
=
0.000881
 dKH
Tank increase=
1135.6
1 dKH
	​

=0.000881 dKH
📌 Final result

1 mg of CaCO₃ → ~0.00088 dKH in a 300-gallon system

Kalk = cheap Ca + Alk + higher pH  

NaOH = cheap, powerful Alk + pH  sodium hydroxide

CaCl₂ = cheap Ca counterpart to your NaOH  calcium chloride

Mg mix = cheap Mg correction every so often

how to mix

Calcium Chloride Dihydrate (CaCl₂·2H₂O)
(Most hobby stuff is dihydrate; BRS, Dowflake, Pool Hardness Increaser)

RO/DI water

Mixing Ratio

500 grams CaCl₂·2H₂O + RO/DI water to make 1 gallon (3.785 L)  i woould need 500grams which is a pound per gallon of water.  i get 7 lbs at amazon for $30


So this is my main 3 AMEN


So your idea (kalk + NaOH + CaCl₂) is actually smart:

handles salinity better than straight 2-part

cheaper than AFR

predictable

good for SPS uptake rates


firebase deploy

////////////////notifications //////////////////////////

on iphone insall ntfi:
////////////////////////////////////////////////////////////////////////////////////////

Now vesion 3 dosing Calcium Chloride, Sodium hydroxide.

mix CaCl2 anhydrous = ~116 g per liter of RO/DI
mix NaOH (Sodium Hydroxide)~84.4 g NaOH per liter

	Code:  float CA_PPM_PER_ML_CACL2_TANK = 0.037f; , float DKH_PER_ML_NAOH_TANK = 0.0052f;

	patch to fix:
	Existing key / pump label	Now means	Element
kalk (Pump 1)	Kalkwasser	Alk + a little Ca
afr (Pump 2)	Calcium Chloride (CaCl2)	Ca only
mg (Pump 3)	Sodium Hydroxide (NaOH)	Alk only
tbd (Pump 4)	Magnesium solution	Mg only

Important note (so we don’t “invent dosing”)

For Mode 2 and Mode 3, I set the new chemistry constants to 0 by default, so AI will not start changing those solutions until you set real “ppm or dKH per ml” values for your specific mixes.

That means:

AI will still work (and won’t crash), but in Mode 2/3 it will mostly push alk toward kalk unless you set the other constants.

Once you set the constants, AI will fully use NaOH/BRS Alk for alk, NaCl/BRS Ca for calcium, etc.

///////////////////////////automated test//////////////////////////////////////////////
C:\Users\mdroo\OneDrive\Documents\platformio\AIDoser\webstuff>python test.py

when you run this test you are testing the 6 modes.
mode 1: Kalk  pump 1 runs.
mode 2 AFR  pump 1 runs.
mode 3 Kalk AFR Mb  pump 1-3 runs
mode 4 Alk Calc Mg  pump 1-3 runs
mode 5 kalk Alk Calc Mb  pump 1-4 run
mode 6 Kalk Calcium Chloride, Sodium Hydroxide, Mg pump 1-4 run
/////////////////////////////////////////////////////////////////////////////////////////