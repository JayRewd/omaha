Project: Omaha headphone HRTF data
==================================

SADIE_KU100_DFC_256_order_fir_48000.sofa
  Neumann KU100 dummy head, diffuse-field compensated, 256-tap FIR, 48 kHz (AES69 SOFA).
  From the SADIE database, University of York (https://www.york.ac.uk/sadie-project/).
  Zenodo record 12542676 (https://zenodo.org/records/12542676).
  Licensed under the Apache License 2.0; see SADIE_LICENSE.txt.

  Citation:
    C. Armstrong, L. Thresh, D. Murphy, G. Kearney, "A Perceptual Evaluation of
    Individual and Non-Individual HRTFs: A Case Study of the SADIE II Database",
    Applied Sciences 8(11):2029, 2018. https://doi.org/10.3390/app8112029

Used by the omaha sound backend when s_speaker_type is 1 (headphones).
Set s_hrtf_file to another .sofa path (relative to the game data search path)
to use a different HRTF set.
