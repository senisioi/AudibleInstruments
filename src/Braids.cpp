#include <string.h>
#include "AudibleInstruments.hpp"
#include "dsp/resampler.hpp"
#include "dsp/ringbuffer.hpp"
#include "braids/macro_oscillator.h"
#include "braids/vco_jitter_source.h"
#include "braids/signature_waveshaper.h"


struct Braids : Module {
	enum ParamIds {
		FINE_PARAM,
		COARSE_PARAM,
		FM_PARAM,
		TIMBRE_PARAM,
		MODULATION_PARAM,
		COLOR_PARAM,
		SHAPE_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		TRIG_INPUT,
		PITCH_INPUT,
		FM_INPUT,
		TIMBRE_INPUT,
		COLOR_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		OUT_OUTPUT,
		NUM_OUTPUTS
	};

	braids::MacroOscillator osc;
	braids::SettingsData settings;
	braids::VcoJitterSource jitter_source;
	braids::SignatureWaveshaper ws;

	dsp::SampleRateConverter<1> src;
	dsp::DoubleRingBuffer<dsp::Frame<1>, 256> outputBuffer;
	bool lastTrig = false;
	bool lowCpu = false;

	Braids();
	void process(const ProcessArgs &args) override;
	void setShape(int shape);

	json_t *dataToJson() override {
		json_t *rootJ = json_object();
		json_t *settingsJ = json_array();
		uint8_t *settingsArray = &settings.shape;
		for (int i = 0; i < 20; i++) {
			json_t *settingJ = json_integer(settingsArray[i]);
			json_array_insert_new(settingsJ, i, settingJ);
		}
		json_object_set_new(rootJ, "settings", settingsJ);

		json_t *lowCpuJ = json_boolean(lowCpu);
		json_object_set_new(rootJ, "lowCpu", lowCpuJ);

		return rootJ;
	}

	void dataFromJson(json_t *rootJ) override {
		json_t *settingsJ = json_object_get(rootJ, "settings");
		if (settingsJ) {
			uint8_t *settingsArray = &settings.shape;
			for (int i = 0; i < 20; i++) {
				json_t *settingJ = json_array_get(settingsJ, i);
				if (settingJ)
					settingsArray[i] = json_integer_value(settingJ);
			}
		}

		json_t *lowCpuJ = json_object_get(rootJ, "lowCpu");
		if (lowCpuJ) {
			lowCpu = json_boolean_value(lowCpuJ);
		}
	}
};


Braids::Braids() {
	config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS);

	params[Braids::SHAPE_PARAM].config(0.0, 1.0, 0.0, "Shape");
	params[Braids::FINE_PARAM].config(-1.0, 1.0, 0.0, "Fine");
	params[Braids::COARSE_PARAM].config(-2.0, 2.0, 0.0, "Coarse");
	params[Braids::FM_PARAM].config(-1.0, 1.0, 0.0, "FM");
	params[Braids::TIMBRE_PARAM].config(0.0, 1.0, 0.5, "Timbre");
	params[Braids::MODULATION_PARAM].config(-1.0, 1.0, 0.0, "Modulation");
	params[Braids::COLOR_PARAM].config(0.0, 1.0, 0.5, "Color");

	memset(&osc, 0, sizeof(osc));
	osc.Init();
	memset(&jitter_source, 0, sizeof(jitter_source));
	jitter_source.Init();
	memset(&ws, 0, sizeof(ws));
	ws.Init(0x0000);
	memset(&settings, 0, sizeof(settings));

	// List of supported settings
	settings.meta_modulation = 0;
	settings.vco_drift = 0;
	settings.signature = 0;
}

void Braids::process(const ProcessArgs &args) {
	// Trigger
	bool trig = inputs[TRIG_INPUT].getVoltage() >= 1.0;
	if (!lastTrig && trig) {
		osc.Strike();
	}
	lastTrig = trig;

	// Render frames
	if (outputBuffer.empty()) {
		float fm = params[FM_PARAM].getValue() * inputs[FM_INPUT].getVoltage();

		// Set shape
		int shape = roundf(params[SHAPE_PARAM].getValue() * braids::MACRO_OSC_SHAPE_LAST_ACCESSIBLE_FROM_META);
		if (settings.meta_modulation) {
			shape += roundf(fm / 10.0 * braids::MACRO_OSC_SHAPE_LAST_ACCESSIBLE_FROM_META);
		}
		settings.shape = clamp(shape, 0, braids::MACRO_OSC_SHAPE_LAST_ACCESSIBLE_FROM_META);

		// Setup oscillator from settings
		osc.set_shape((braids::MacroOscillatorShape) settings.shape);

		// Set timbre/modulation
		float timbre = params[TIMBRE_PARAM].getValue() + params[MODULATION_PARAM].getValue() * inputs[TIMBRE_INPUT].getVoltage() / 5.0;
		float modulation = params[COLOR_PARAM].getValue() + inputs[COLOR_INPUT].getVoltage() / 5.0;
		int16_t param1 = rescale(clamp(timbre, 0.0f, 1.0f), 0.0f, 1.0f, 0, INT16_MAX);
		int16_t param2 = rescale(clamp(modulation, 0.0f, 1.0f), 0.0f, 1.0f, 0, INT16_MAX);
		osc.set_parameters(param1, param2);

		// Set pitch
		float pitchV = inputs[PITCH_INPUT].getVoltage() + params[COARSE_PARAM].getValue() + params[FINE_PARAM].getValue() / 12.0;
		if (!settings.meta_modulation)
			pitchV += fm;
		if (lowCpu)
			pitchV += log2f(96000.f * args.sampleTime);
		int32_t pitch = (pitchV * 12.0 + 60) * 128;
		pitch += jitter_source.Render(settings.vco_drift);
		pitch = clamp(pitch, 0, 16383);
		osc.set_pitch(pitch);

		// TODO: add a sync input buffer (must be sample rate converted)
		uint8_t sync_buffer[24] = {};

		int16_t render_buffer[24];
		osc.Render(sync_buffer, render_buffer, 24);

		// Signature waveshaping, decimation (not yet supported), and bit reduction (not yet supported)
		uint16_t signature = settings.signature * settings.signature * 4095;
		for (size_t i = 0; i < 24; i++) {
			const int16_t bit_mask = 0xffff;
			int16_t sample = render_buffer[i] & bit_mask;
			int16_t warped = ws.Transform(sample);
			render_buffer[i] = stmlib::Mix(sample, warped, signature);
		}

		if (lowCpu) {
			for (int i = 0; i < 24; i++) {
				dsp::Frame<1> f;
				f.samples[0] = render_buffer[i] / 32768.0;
				outputBuffer.push(f);
			}
		}
		else {
			// Sample rate convert
			dsp::Frame<1> in[24];
			for (int i = 0; i < 24; i++) {
				in[i].samples[0] = render_buffer[i] / 32768.0;
			}
			src.setRates(96000, args.sampleRate);

			int inLen = 24;
			int outLen = outputBuffer.capacity();
			src.process(in, &inLen, outputBuffer.endData(), &outLen);
			outputBuffer.endIncr(outLen);
		}
	}

	// Output
	if (!outputBuffer.empty()) {
		dsp::Frame<1> f = outputBuffer.shift();
		outputs[OUT_OUTPUT].setVoltage(5.0 * f.samples[0]);
	}
}


static const char *algo_values[] = {
	"CSAW",
	"/\\-_",
	"//-_",
	"FOLD",
	"uuuu",
	"SUB-",
	"SUB/",
	"SYN-",
	"SYN/",
	"//x3",
	"-_x3",
	"/\\x3",
	"SIx3",
	"RING",
	"////",
	"//uu",
	"TOY*",
	"ZLPF",
	"ZPKF",
	"ZBPF",
	"ZHPF",
	"VOSM",
	"VOWL",
	"VFOF",
	"HARM",
	"FM  ",
	"FBFM",
	"WTFM",
	"PLUK",
	"BOWD",
	"BLOW",
	"FLUT",
	"BELL",
	"DRUM",
	"KICK",
	"CYMB",
	"SNAR",
	"WTBL",
	"WMAP",
	"WLIN",
	"WTx4",
	"NOIS",
	"TWNQ",
	"CLKN",
	"CLOU",
	"PRTC",
	"QPSK",
	"    ",
};

struct BraidsDisplay : TransparentWidget {
	Braids *module;
	std::shared_ptr<Font> font;

	BraidsDisplay() {
		font = APP->window->loadFont(asset::plugin(pluginInstance, "res/hdad-segment14-1.002/Segment14.ttf"));
	}

	void draw(NVGcontext *vg) override {
	    int shape = 0 ;
	    if (module)
		    shape = module->settings.shape;

		// Background
		NVGcolor backgroundColor = nvgRGB(0x38, 0x38, 0x38);
		NVGcolor borderColor = nvgRGB(0x10, 0x10, 0x10);
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0.0, 0.0, box.size.x, box.size.y, 5.0);
		nvgFillColor(vg, backgroundColor);
		nvgFill(vg);
		nvgStrokeWidth(vg, 1.0);
		nvgStrokeColor(vg, borderColor);
		nvgStroke(vg);

		nvgFontSize(vg, 36);
		nvgFontFaceId(vg, font->handle);
		nvgTextLetterSpacing(vg, 2.5);

		Vec textPos = Vec(10, 48);
		NVGcolor textColor = nvgRGB(0xaf, 0xd2, 0x2c);
		nvgFillColor(vg, nvgTransRGBA(textColor, 16));
		nvgText(vg, textPos.x, textPos.y, "~~~~", NULL);
		nvgFillColor(vg, textColor);
		nvgText(vg, textPos.x, textPos.y, algo_values[shape], NULL);
	}
};


struct BraidsSettingItem : MenuItem {
	uint8_t *setting = NULL;
	uint8_t offValue = 0;
	uint8_t onValue = 1;
	void onAction(const ActionEvent &e) override {
		// Toggle setting
		*setting = (*setting == onValue) ? offValue : onValue;
	}
	void step() override {
		rightText = (*setting == onValue) ? "✔" : "";
		MenuItem::step();
	}
};

struct BraidsLowCpuItem : MenuItem {
	Braids *braids;
	void onAction(const ActionEvent &e) override {
		braids->lowCpu = !braids->lowCpu;
	}
	void step() override {
		rightText = (braids->lowCpu) ? "✔" : "";
		MenuItem::step();
	}
};


struct BraidsWidget : ModuleWidget {
	BraidsWidget(Braids *module) {
		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Braids.svg")));

		{
			BraidsDisplay *display = new BraidsDisplay();
			display->box.pos = Vec(14, 53);
			display->box.size = Vec(148, 56);
			display->module = module;
			addChild(display);
		}

		addChild(createWidget<ScrewSilver>(Vec(15, 0)));
		addChild(createWidget<ScrewSilver>(Vec(210, 0)));
		addChild(createWidget<ScrewSilver>(Vec(15, 365)));
		addChild(createWidget<ScrewSilver>(Vec(210, 365)));

		addParam(createParam<Rogan2SGray>(Vec(176, 59), module, Braids::SHAPE_PARAM));

		addParam(createParam<Rogan2PSWhite>(Vec(19, 138), module, Braids::FINE_PARAM));
		addParam(createParam<Rogan2PSWhite>(Vec(97, 138), module, Braids::COARSE_PARAM));
		addParam(createParam<Rogan2PSWhite>(Vec(176, 138), module, Braids::FM_PARAM));

		addParam(createParam<Rogan2PSGreen>(Vec(19, 217), module, Braids::TIMBRE_PARAM));
		addParam(createParam<Rogan2PSGreen>(Vec(97, 217), module, Braids::MODULATION_PARAM));
		addParam(createParam<Rogan2PSRed>(Vec(176, 217), module, Braids::COLOR_PARAM));

		addInput(createInput<PJ301MPort>(Vec(10, 316), module, Braids::TRIG_INPUT));
		addInput(createInput<PJ301MPort>(Vec(47, 316), module, Braids::PITCH_INPUT));
		addInput(createInput<PJ301MPort>(Vec(84, 316), module, Braids::FM_INPUT));
		addInput(createInput<PJ301MPort>(Vec(122, 316), module, Braids::TIMBRE_INPUT));
		addInput(createInput<PJ301MPort>(Vec(160, 316), module, Braids::COLOR_INPUT));
		addOutput(createOutput<PJ301MPort>(Vec(205, 316), module, Braids::OUT_OUTPUT));
	}

	void appendContextMenu(Menu *menu) override {
		Braids *braids = dynamic_cast<Braids*>(module);
		assert(braids);

		menu->addChild(construct<MenuLabel>());
		menu->addChild(construct<MenuLabel>(&MenuLabel::text, "Options"));
		menu->addChild(construct<BraidsSettingItem>(&MenuItem::text, "META", &BraidsSettingItem::setting, &braids->settings.meta_modulation));
		menu->addChild(construct<BraidsSettingItem>(&MenuItem::text, "DRFT", &BraidsSettingItem::setting, &braids->settings.vco_drift, &BraidsSettingItem::onValue, 4));
		menu->addChild(construct<BraidsSettingItem>(&MenuItem::text, "SIGN", &BraidsSettingItem::setting, &braids->settings.signature, &BraidsSettingItem::onValue, 4));
		menu->addChild(construct<BraidsLowCpuItem>(&MenuItem::text, "Low CPU", &BraidsLowCpuItem::braids, braids));
	}
};


Model *modelBraids = createModel<Braids, BraidsWidget>("Braids");
