#include "CalibrationBridgeDialog.hpp"
#include "I18N.hpp"
#include "libslic3r/Utils.hpp"
#include "GUI.hpp"
#include "GUI_ObjectList.hpp"
#include "Tab.hpp"
#include <wx/scrolwin.h>
#include <wx/display.h>
#include <wx/file.h>
#include "wxExtensions.hpp"

#if ENABLE_SCROLLABLE
static wxSize get_screen_size(wxWindow* window)
{
    const auto idx = wxDisplay::GetFromWindow(window);
    wxDisplay display(idx != wxNOT_FOUND ? idx : 0u);
    return display.GetClientArea().GetSize();
}
#endif // ENABLE_SCROLLABLE

namespace Slic3r {
namespace GUI {

void CalibrationBridgeDialog::create_buttons(wxStdDialogButtonSizer* buttons){
    wxString choices_steps[] = { "2.5","5","10" };
    steps = new wxComboBox(this, wxID_ANY, wxString{ "5" }, wxDefaultPosition, wxDefaultSize, 3, choices_steps);
    steps->SetToolTip(_(L("Select the step in % between two tests.")));
    steps->SetSelection(1);
    wxString choices_nb[] = { "1","2","3","4","5","6" };
    nb_tests = new wxComboBox(this, wxID_ANY, wxString{ "5" }, wxDefaultPosition, wxDefaultSize, 6, choices_nb);
    nb_tests->SetToolTip(_(L("Select the number of tests")));
    nb_tests->SetSelection(4);

    buttons->Add(new wxStaticText(this, wxID_ANY, wxString{ "step:" }));
    buttons->Add(steps);
    buttons->AddSpacer(15);
    buttons->Add(new wxStaticText(this, wxID_ANY, wxString{ "nb tests:" }));
    buttons->Add(nb_tests);
    buttons->AddSpacer(40);
    wxButton* bt = new wxButton(this, wxID_FILE1, _(L("Test Flow Ratio")));
    bt->Bind(wxEVT_BUTTON, &CalibrationBridgeDialog::create_geometry_flow_ratio, this);
    buttons->Add(bt);
    buttons->AddSpacer(15);
    bt = new wxButton(this, wxID_FILE1, _(L("Test Overlap")));
    bt->Bind(wxEVT_BUTTON, &CalibrationBridgeDialog::create_geometry_overlap, this);
    buttons->Add(bt);
}

void CalibrationBridgeDialog::create_geometry(std::string setting_to_test, bool add) {
    Plater* plat = this->main_frame->plater();
    Model& model = plat->model();
    plat->reset();

    int idx_steps = steps->GetSelection();
    int idx_nb = nb_tests->GetSelection();
    size_t step = 5 + (idx_steps == wxNOT_FOUND ? 0 : (idx_steps == 0 ? 2.5f : idx_steps == 1 ? 5.F : 10.f));
    size_t nb_items = 1 + (idx_nb == wxNOT_FOUND ? 0 : idx_nb);

    std::vector<std::string> items;
    for (size_t i = 0; i < nb_items; i++)
        items.emplace_back("./resources/calibration/bridge_flow/bridge_test.amf");
    std::vector<size_t> objs_idx = plat->load_files(items, true, false);

    assert(objs_idx.size() == nb_items);
    const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_PRINT)->get_config();
    const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();

    /// --- scale ---
    // model is created for a 0.4 nozzle, scale xy with nozzle size.
    const ConfigOptionFloats* nozzle_diameter_config = printer_config->option<ConfigOptionFloats>("nozzle_diameter");
    assert(nozzle_diameter_config->values.size() > 0);
    float nozzle_diameter = nozzle_diameter_config->values[0];
    float z_scale = nozzle_diameter / 0.4;
    //do scaling
    if (z_scale < 0.9 || 1.2 < z_scale) {
        for (size_t i = 0; i < 5; i++)
            model.objects[objs_idx[i]]->scale(1, 1, z_scale);
    } else {
        z_scale = 1;
    }

    //add sub-part after scale
    const ConfigOptionPercent* bridge_flow_ratio = print_config->option<ConfigOptionPercent>(setting_to_test);
    int start = bridge_flow_ratio->value;
    float zshift = 2.3 * (1 - z_scale);
    for (size_t i = 0; i < nb_items; i++) {
        if((start + (add ? 1 : -1) * i * step) < 180 && start + (start + (add ? 1 : -1) * i * step) > 20)
            add_part(model.objects[objs_idx[i]], Slic3r::resources_dir() + "/calibration/bridge_flow/f"+std::to_string(start + (add ? 1 : -1) * i * step)+".amf", Vec3d{ -10,0, zshift + 4.6 * z_scale }, Vec3d{ 1,1,z_scale });
    }

    /// --- translate ---;
    const ConfigOptionFloat* extruder_clearance_radius = print_config->option<ConfigOptionFloat>("extruder_clearance_radius");
    const ConfigOptionPoints* bed_shape = printer_config->option<ConfigOptionPoints>("bed_shape");
    Vec2d bed_size = BoundingBoxf(bed_shape->values).size();
    Vec2d bed_min = BoundingBoxf(bed_shape->values).min;
    float offsety = 5 + extruder_clearance_radius->value + 10;
    model.objects[objs_idx[0]]->translate({ bed_min.x() + bed_size.x() / 2, bed_min.y() + bed_size.y() / 2, 0 });
    for (int i = 1; i < nb_items; i++) {
        model.objects[objs_idx[i]]->translate({ bed_min.x() + bed_size.x() / 2, bed_min.y() + bed_size.y() / 2 + (i%2==0?-1:1) * offsety * ((i+1)/2), 0 });
    }
    //TODO: if not enough space, forget about complete_objects


    /// --- main config, please modify object config when possible ---
    DynamicPrintConfig new_print_config = *print_config; //make a copy
    new_print_config.set_key_value("complete_objects", new ConfigOptionBool(true));

    /// --- custom config ---
    for (size_t i = 0; i < nb_items; i++) {
        model.objects[objs_idx[i]]->config.set_key_value("perimeters", new ConfigOptionInt(2));
        model.objects[objs_idx[i]]->config.set_key_value("bottom_solid_layers", new ConfigOptionInt(2));
        model.objects[objs_idx[i]]->config.set_key_value("gap_fill", new ConfigOptionBool(false));
        model.objects[objs_idx[i]]->config.set_key_value(setting_to_test, new ConfigOptionPercent(start + (add ? 1 : -1) * i * step));
        model.objects[objs_idx[i]]->config.set_key_value("layer_height", new ConfigOptionFloat(nozzle_diameter / 2));
        model.objects[objs_idx[i]]->config.set_key_value("no_perimeter_unsupported_algo", new ConfigOptionEnum<NoPerimeterUnsupportedAlgo>(npuaBridges));
        model.objects[objs_idx[i]]->config.set_key_value("top_fill_pattern", new ConfigOptionEnum<InfillPattern>(ipSmooth));
    }

    //update plater
    this->gui_app->get_tab(Preset::TYPE_PRINT)->load_config(new_print_config);
    plat->on_config_change(new_print_config);
    plat->changed_objects(objs_idx);
    //this->gui_app->get_tab(Preset::TYPE_PRINT)->update_dirty();
    //update everything, easier to code.
    ObjectList* obj = this->gui_app->obj_list();
    obj->update_after_undo_redo();


    plat->reslice();
    plat->select_view_3D("Preview");
}

} // namespace GUI
} // namespace Slic3r
