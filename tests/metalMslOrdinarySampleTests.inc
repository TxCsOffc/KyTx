// Ordinary image_sample offsets are separate from Dref. Use both CPU texel
// footprints and the production SPIR-V -> optimizer -> SPIRV-Cross GPU path.
static void OrdinarySampleOffsets() {
	using D=ShaderRecompiler::Decoder::ImageDimension;
	unsigned checked=0, blended=0, reference_expected=0;
#if KYTX_COMPARE_SPIRV_PATH
	unsigned references=0;
#endif
	for (auto dimension : {D::Dim2D,D::Dim2DArray,D::Dim3D}) for (bool gradient : {false,true}) {
		ComparisonHarness h(dimension,false,true,gradient,MTLPixelFormatR32Float,false);
		for (auto offset : {std::array{-1,1,-1},std::array{2,-2,1},std::array{-32,31,-32}})
		for (bool linear : {false,true}) for (float lod : {0.f,.75f,1.5f}) for (unsigned clamp : {0u,1u,2u}) {
			ComparisonCase c;c.offset=offset;c.min_linear=c.mag_linear=linear;c.lod=lod;c.mip=2;c.clamp=clamp;
			c.u=.37f;c.v=.68f;c.z=.21f;c.slice=h.array?1:0;h.Run(c,blended);++checked;
			bool legal=true;for (unsigned axis=0;axis<h.spatial;++axis) legal &= offset[axis]>=-8 && offset[axis]<=7;
			reference_expected+=legal;
		}
		for (unsigned reduction : {0u,1u,2u}) for (bool min : {false,true}) for (bool mag : {false,true})
		for (float lod : {.25f,1.5f}) for (int bias : {-128,128}) {
			ComparisonCase c;c.offset={-32,31,-32};c.reduction=reduction;c.min_linear=min;c.mag_linear=mag;
			// log2 of the uploaded float gradient can be 1.49999988 rather
			// than nominal 1.5. Min/max includes any nonzero mip weight, so
			// keep gradient probes away from that discontinuity; explicit LOD
			// probes above still cover the exact integer boundary.
			c.lod=gradient && lod==1.5f?1.375f:lod;c.bias=bias;c.mip=2;c.min_lod=128;c.max_lod=384;c.slice=h.array?1:0;
			c.u=.37f;c.v=.68f;c.z=.21f;h.Run(c,blended);++checked;
		}
		for (unsigned reduction : {1u,2u}) {
			ComparisonCase c;c.offset={-32,31,-32};c.reduction=reduction;c.mip=0;c.clamp=0;
			c.u=.375f;c.v=.625f;c.z=.375f;c.slice=h.array?1:0;h.Run(c,blended);++checked;
		}
		for (bool mag : {false,true}) {
			ComparisonCase c;c.offset={-32,31,-32};c.pixel=true;c.min_linear=!mag;c.mag_linear=mag;c.mip=2;c.lod=1.5f;
			c.u=1.7f;c.v=2.1f;c.z=3.3f;c.slice=h.array?1:0;h.Run(c,blended);++checked;
		}
#if KYTX_COMPARE_SPIRV_PATH
		references+=h.reference_cases;
#endif
	}
	std::printf("Ordinary sample offsets: %u GPU cases (2D/array/3D, LOD/gradients, signed 6-bit offsets) CPU/guards PASS\n",checked);
#if KYTX_COMPARE_SPIRV_PATH
	Check(references==reference_expected,"ordinary shader-path reference did not run every host-legal offset case");
	std::printf("Ordinary sample offsets: %u matching production SPIR-V->MSL GPU cases in the host offset range\n",references);
#endif
	unsigned dref=0;
	for (auto dimension : {D::Dim2D,D::Dim2DArray}) {
		const auto packed=32u|(31u<<8u);
		ComparisonHarness h(dimension,false,true,false,MTLPixelFormatR32Float,true,packed);
		for (unsigned func=0;func<8;++func) for (bool linear : {false,true}) for (float lod : {0.f,.75f,1.5f}) {
			ComparisonCase c;c.offset={-32,31,0};c.function=func;c.min_linear=c.mag_linear=linear;c.mip=2;c.lod=lod;
			c.u=.37f;c.v=.68f;c.slice=h.array?1:0;h.Run(c,blended);++dref;
		}
	}
	std::printf("Constant Dref offsets outside Metal's four-bit range: %u CPU/GPU cases PASS\n",dref);
}
